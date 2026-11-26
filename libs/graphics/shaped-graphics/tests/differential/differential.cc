#include "differential.hh"

#include "../shaders/shader_fixtures.hh"

#include <clean-core/math/bit.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sg_test_differential.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/capabilities.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/metrics.hh>
#include <shaped-shader-library/compiler/shader_compiler.hh>
#include <shaped-shader-library/shader_asset.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-shader-library/shader_package.hh>

using namespace cc::primitive_defines;

namespace
{
constexpr cc::string_view k_marker = "// differential:";
/// How many differing elements one output names before it only counts the rest.
constexpr isize k_named_elements = 4;

[[nodiscard]] cc::string_view trimmed(cc::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s = s.subview(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s = s.subview({.offset = 0, .size = s.size() - 1});
    return s;
}

[[nodiscard]] bool is_nan_bits(u32 bits)
{
    return (bits & 0x7f80'0000u) == 0x7f80'0000u && (bits & 0x007f'ffffu) != 0;
}

/// The float's bits on one line from the most negative to the most positive, so two floats' distance is their ulps apart.
[[nodiscard]] i64 ordered(u32 bits)
{
    return (bits & 0x8000'0000u) != 0 ? -i64(bits & 0x7fff'ffffu) : i64(bits);
}

[[nodiscard]] bool within(u32 a, u32 b, sg_diff::tolerance t)
{
    if (a == b)
        return true;
    if (t.kind == sg_diff::tolerance::measure::exact)
        return false;
    if (is_nan_bits(a) || is_nan_bits(b))
        return is_nan_bits(a) && is_nan_bits(b);
    if (t.kind == sg_diff::tolerance::measure::ulps)
    {
        auto const apart = ordered(a) - ordered(b);
        return double(apart < 0 ? -apart : apart) <= t.amount;
    }
    auto const fa = double(cc::bit_cast<f32>(a));
    auto const fb = double(cc::bit_cast<f32>(b));
    // an infinity is near only itself, which the bit compare above has already said
    if (fa - fa != 0.0 || fb - fb != 0.0)
        return false;
    auto const scale = cc::max(1.0, cc::max(fa < 0 ? -fa : fa, fb < 0 ? -fb : fb));
    auto const delta = fa - fb;
    return (delta < 0 ? -delta : delta) <= t.amount * scale;
}

/// How much of a tolerance of `measure` holding `b` to `a` takes: 0 where they are equal, infinity where none does.
[[nodiscard]] double needed(u32 a, u32 b, sg_diff::tolerance::measure measure)
{
    if (a == b)
        return 0;
    auto const never = cc::bit_cast<f64>(0x7ff0'0000'0000'0000ull);
    if (measure == sg_diff::tolerance::measure::exact || is_nan_bits(a) || is_nan_bits(b))
        return is_nan_bits(a) && is_nan_bits(b) ? 0 : never;
    if (measure == sg_diff::tolerance::measure::ulps)
    {
        auto const apart = ordered(a) - ordered(b);
        return double(apart < 0 ? -apart : apart);
    }
    auto const fa = double(cc::bit_cast<f32>(a));
    auto const fb = double(cc::bit_cast<f32>(b));
    if (fa - fa != 0.0 || fb - fb != 0.0)
        return never;
    auto const scale = cc::max(1.0, cc::max(fa < 0 ? -fa : fa, fb < 0 ? -fb : fb));
    auto const delta = fa - fb;
    return (delta < 0 ? -delta : delta) / scale;
}

/// Whether `b` is within `t` of the range `least` to `most`; a range with an infinite end bounds nothing.
/// Held `exact` where the range is no hull, `b` must be one of the ends, bit for bit: the range is then a conversion's
/// two roundings, and a value between those is no rounding of the conversion.
[[nodiscard]] bool within_range(f32 least, f32 most, u32 b, sg_diff::tolerance t, bool is_hull)
{
    if (least - least != 0.0f || most - most != 0.0f)
        return true;
    if (t.kind == sg_diff::tolerance::measure::exact && !is_hull)
        return b == cc::bit_cast<u32>(least) || b == cc::bit_cast<u32>(most);
    if (is_nan_bits(b))
        return false;
    auto const fb = cc::bit_cast<f32>(b);
    if (fb >= least && fb <= most)
        return true;
    return within(cc::bit_cast<u32>(fb < least ? least : most), b, t);
}

/// Whether two GPU results `a` and `b` of one element both lie within `t` of the range `ranges` allows it.
/// An element `ranges` bounds by nothing, or does not bound, holds them to each other alone.
[[nodiscard]] bool both_in_range(
    sg_diff::run_result const* ranges, isize output, cc::string_view name, isize e, u32 a, u32 b, sg_diff::tolerance t)
{
    if (ranges == nullptr || output >= ranges->outputs.size() || ranges->outputs[output].name != name)
        return false;
    auto const& bounds = ranges->outputs[output].bounds;
    if (bounds.size() <= 2 * e + 1)
        return false;
    auto const least = bounds[2 * e];
    auto const most = bounds[2 * e + 1];
    if (least - least != 0.0f || most - most != 0.0f)
        return false;
    return within_range(least, most, a, t, ranges->ranges_are_hulls)
        && within_range(least, most, b, t, ranges->ranges_are_hulls);
}

[[nodiscard]] cc::string text_of(u32 bits, sg_diff::tolerance t)
{
    if (t.kind == sg_diff::tolerance::measure::exact)
        return cc::format("0x{:08x}", bits);
    return cc::format("{} (0x{:08x})", cc::bit_cast<f32>(bits), bits);
}
} // namespace

namespace
{
/// `nvidia vulkan bug-...`: a vendor by its PCI id, a backend, and the issue that explains the divergence.
/// The vendor may name its drivers, `amd/mesa` for Mesa's or `amd/own` for AMD's own, which tell RADV from AMD's.
[[nodiscard]] cc::optional<sg_diff::known_divergence> known_divergence_of(cc::string_view text)
{
    auto words = cc::vector<cc::string_view>();
    while (!text.empty())
    {
        auto end = isize(0);
        while (end < text.size() && text[end] != ' ')
            ++end;
        words.push_back(text.subview({.offset = 0, .size = end}));
        text = trimmed(end < text.size() ? text.subview(end) : cc::string_view());
    }
    if (words.size() != 3 && !(words.size() == 4 && (words[3] == "precise" || words[3] == "across")))
        return cc::nullopt;

    auto result = sg_diff::known_divergence{.issue = cc::string(words[2])};
    if (words.size() == 4)
        result.scope = cc::string(words[3]);
    auto vendor = words[0];
    if (vendor.ends_with("/mesa"))
    {
        result.driver = sg_diff::known_divergence::drivers::mesa;
        vendor = vendor.subview({.offset = 0, .size = vendor.size() - 5});
    }
    else if (vendor.ends_with("/own"))
    {
        result.driver = sg_diff::known_divergence::drivers::own;
        vendor = vendor.subview({.offset = 0, .size = vendor.size() - 4});
    }
    if (vendor == "amd")
        result.vendor_id = 0x1002;
    else if (vendor == "apple")
        result.vendor_id = 0x106B;
    else if (vendor == "intel")
        result.vendor_id = 0x8086;
    else if (vendor == "nvidia")
        result.vendor_id = 0x10DE;
    else
        return cc::nullopt;

    if (words[1] == "dx12")
        result.backend = sg::backend_kind::dx12;
    else if (words[1] == "vulkan")
        result.backend = sg::backend_kind::vulkan;
    else if (words[1] == "metal")
        result.backend = sg::backend_kind::metal;
    else if (words[1] == "webgpu")
        result.backend = sg::backend_kind::webgpu;
    else
        return cc::nullopt;
    return result;
}
} // namespace

sg_diff::known_divergence const* sg_diff::workload::known_on(sg::context const& ctx, cc::string_view scope) const
{
    auto const& adapter = ctx.metrics.adapter();
    auto const is_mesa = cc::string_view(adapter.driver_info).starts_with("Mesa");
    for (auto const& k : known)
    {
        auto const drives
            = k.driver == known_divergence::drivers::any || (k.driver == known_divergence::drivers::mesa) == is_mesa;
        if (k.vendor_id == adapter.vendor_id && drives && k.backend == ctx.backend() && k.scope == scope)
            return &k;
    }
    return nullptr;
}

sg_diff::known_divergence const* sg_diff::workload::known_between_gpus(sg::context const& ctx) const
{
    auto const* const across = known_on(ctx, "across");
    return across != nullptr ? across : known_on(ctx);
}

cc::optional<sg_diff::tolerance> sg_diff::tolerance::parse(cc::string_view text)
{
    text = trimmed(text);
    if (text == "exact")
        return tolerance{};
    auto const with_amount = [&](cc::string_view word, measure k) -> cc::optional<tolerance>
    {
        auto const amount = cc::from_string<double>(trimmed(text.subview(word.size())));
        if (!amount.has_value() || amount.value() < 0)
            return cc::nullopt;
        return tolerance{.kind = k, .amount = amount.value()};
    };
    if (text.starts_with("ulps "))
        return with_amount("ulps", measure::ulps);
    if (text.starts_with("relative "))
        return with_amount("relative", measure::relative);
    return cc::nullopt;
}

cc::string sg_diff::tolerance::to_string() const
{
    switch (kind)
    {
    case measure::exact:
        return "exact";
    case measure::ulps:
        return cc::format("ulps {}", amount);
    case measure::relative:
        return cc::format("relative {}", amount);
    }
    return {};
}

cc::vector<sg_diff::setup>& sg_diff::setups()
{
    static cc::vector<setup> entries;
    return entries;
}

bool sg_diff::register_setup(setup s)
{
    setups().push_back(cc::move(s));
    return true;
}

sg_diff::corpus sg_diff::read_corpus()
{
    auto result = corpus();
    (void)sg_test::shader_fixtures(); // fills the package's handles

    // `SC_DIFFERENTIAL_ONLY` narrows the corpus to the files whose path holds it, to chase one file on one machine
    auto const only = cc::environment_variable("SC_DIFFERENTIAL_ONLY");
    for (auto const& def : sg::test::differential::package().definitions)
    {
        if (def.asset == nullptr || *def.asset == nullptr)
            continue;
        if (only.has_value() && !only.value().empty() && !cc::string_view(def.path).contains(only.value()))
            continue;
        auto const& asset = *def.asset;
        auto const source = asset->read_source();
        if (!source.has_value())
        {
            result.problems.push_back(cc::format("{}: the library cannot read it", def.path));
            continue;
        }

        auto required = cc::vector<sg::feature>();
        auto setup_name = cc::string();
        auto routes = cc::optional<tolerance>();
        auto across = cc::optional<tolerance>();
        auto known = cc::vector<known_divergence>();
        auto problem = cc::string();
        auto rest = cc::string_view(source.value());
        while (!rest.empty())
        {
            auto end = isize(0);
            while (end < rest.size() && rest[end] != '\n')
                ++end;
            auto const line = trimmed(rest.subview({.offset = 0, .size = end}));
            rest = end < rest.size() ? rest.subview(end + 1) : cc::string_view();
            if (line.starts_with("require "))
            {
                // SGL's `require` spells a feature as sg::to_string names it, several to a line, comma separated
                for (auto const f : sg::k_all_features)
                {
                    auto const name = sg::to_string(f);
                    auto rest_of_line = trimmed(line.subview(8));
                    while (!rest_of_line.empty())
                    {
                        auto end = isize(0);
                        while (end < rest_of_line.size() && rest_of_line[end] != ',')
                            ++end;
                        if (trimmed(rest_of_line.subview({.offset = 0, .size = end})) == name)
                            required.push_back(f);
                        rest_of_line
                            = end < rest_of_line.size() ? trimmed(rest_of_line.subview(end + 1)) : cc::string_view();
                    }
                }
                continue;
            }
            if (!line.starts_with(k_marker))
                continue;

            auto const statement = trimmed(line.subview(k_marker.size()));
            auto split = isize(0);
            while (split < statement.size() && statement[split] != ' ')
                ++split;
            auto const key = statement.subview({.offset = 0, .size = split});
            auto const value = trimmed(statement.subview(split));
            if (key == "setup")
                setup_name = cc::string(value);
            else if (key == "routes" || key == "across")
            {
                auto const t = tolerance::parse(value);
                if (!t.has_value())
                    problem = cc::format("'{}' is no tolerance; one is `exact`, `ulps <n>` or `relative <x>`", value);
                else if (key == "routes")
                    routes = t;
                else
                    across = t;
            }
            else if (key == "known")
            {
                auto const parsed = known_divergence_of(value);
                if (parsed.has_value())
                    known.push_back(parsed.value());
                else
                    problem = cc::format("'{}' is no known divergence; one is `known <vendor> <backend> <issue>`, the "
                                         "vendor amd, apple, intel or nvidia, with `/mesa` or `/own` for those drivers "
                                         "alone, the backend "
                                         "dx12, vulkan, metal or "
                                         "webgpu, and a trailing `precise` or `across` for the strict-floor twin or "
                                         "the GPU-to-GPU tests alone",
                                         value);
            }
            else
                problem
                    = cc::format("'{}' is no key of a comment block; the keys are setup, routes, across and known", key);
        }

        auto const* found = static_cast<setup const*>(nullptr);
        for (auto const& s : setups())
            if (s.name == setup_name)
                found = &s;
        if (problem.empty() && setup_name.empty())
            problem = cc::format("it names no setup: a `{} setup <name>` line is what puts it in the corpus", k_marker);
        else if (problem.empty() && found == nullptr)
            problem = cc::format("'{}' is no registered setup", setup_name);
        if (!problem.empty())
        {
            // once a file, however many entry points it holds
            auto const said = cc::format("{}: {}", def.path, problem);
            auto is_known = false;
            for (auto const& p : result.problems)
                is_known = is_known || p == said;
            if (!is_known)
                result.problems.push_back(said);
            continue;
        }
        // a stage the setup does not run is the file's own business, a helper stage or a later setup's
        if (def.stage != found->stage)
            continue;

        result.workloads.push_back(workload{
            .file = cc::string(def.path),
            .entry = cc::string(def.entry_point),
            .asset = asset,
            .setup = found,
            .routes = routes.has_value() ? routes.value() : found->routes,
            .across = across.has_value() ? across.value() : found->across,
            .required = cc::move(required),
            .known = cc::move(known),
        });
    }
    return result;
}

cc::vector<sg_diff::route> sg_diff::routes_of(sg::context const& ctx)
{
    auto& lib = sg_test::shader_fixtures();
    for (auto const format : ctx.accepted_shader_formats())
    {
        if (!lib.can_compile(slib::shader_language::sgl, format))
            continue;
        switch (format)
        {
        case sg::shader_format::dxil:
        case sg::shader_format::spirv:
            return {route{.name = "dxc", .format = format}, route{.name = "native", .format = format, .native = true}};
        case sg::shader_format::wgsl:
            return {route{.name = "wgsl", .format = format}};
        case sg::shader_format::metal_lib:
            return {route{.name = "msl", .format = format}};
        default:
            return {route{.name = "other", .format = format}};
        }
    }
    return {};
}

cc::shared_async<sg_diff::run_result> sg_diff::run(sg::context* ctx, workload const* w, route r)
{
    if (auto* const home = ctx->device_home())
        co_await cc::async_resume_on(*home);

    if (w->setup->required.has_value() && !ctx->supports(w->setup->required.value()))
        co_return run_result{.skipped = cc::format("this context lacks what the {} setup requires", w->setup->name)};
    for (auto const f : w->required)
        if (!ctx->supports(f))
            co_return run_result{.skipped
                                 = cc::format("this context lacks {}, which {} requires", sg::to_string(f), w->file)};
    auto const shader = co_await compiled_on(*ctx, r, *w->asset);
    // the bytecode each route ran, to run in a standalone reproduction (../../docs/differential-testing.md)
    if (auto const dir = cc::environment_variable("SC_DIFFERENTIAL_SHADER_DUMP"); dir.has_value())
        if (auto f = cc::file_write_stream_adapter::create(cc::format(
                "{}/{}-{}-{}{}.bin", dir.value(), w->entry, ctx->backend() == sg::backend_kind::dx12 ? "dx12" : "vulkan",
                r.name, r.float_floor.empty() ? cc::string() : "-" + r.float_floor));
            f.has_value())
        {
            auto stream = f.value().stream();
            (void)stream.write(shader.bytecode.span());
            (void)stream.flush();
        }
    if (r.native && shader.compiler.name != "sgl")
        co_return run_result{.skipped = cc::format("SGL writes no {} of it yet, so {} compiled it",
                                                   r.format == sg::shader_format::dxil ? "DXIL" : "SPIR-V",
                                                   shader.compiler.name)};

    co_return run_result{.outputs = co_await w->setup->run(ctx, {.entry = shader, .route = r})};
}

sg::async_compiled_shader sg_diff::compiled_on(sg::context const& ctx, route const& r, slib::shader_asset const& asset)
{
    // the context's own options, with the route stated rather than the context's preference
    auto options = cc::vector<slib::shader_option>();
    slib::add_context_options(options, ctx);
    auto kept = cc::vector<slib::shader_option>();
    for (auto& o : options)
        if (o.name != slib::k_native_dxil_option && o.name != slib::k_native_spirv_option)
            kept.push_back(cc::move(o));
    if (r.native)
        kept.push_back({.name = cc::string(r.format == sg::shader_format::dxil ? slib::k_native_dxil_option
                                                                               : slib::k_native_spirv_option),
                        .value = cc::string("true")});
    if (!r.float_floor.empty())
        kept.push_back({.name = cc::string(slib::k_float_floor_option), .value = r.float_floor});
    return asset.acquire(r.format, kept);
}

cc::vector<cc::string> sg_diff::differences(run_result const& a, run_result const& b, tolerance t, run_result const* ranges)
{
    auto lines = cc::vector<cc::string>();
    if (a.outputs.size() != b.outputs.size())
    {
        lines.push_back(cc::format("{} outputs against {}", b.outputs.size(), a.outputs.size()));
        return lines;
    }
    for (auto i = isize(0); i < a.outputs.size(); ++i)
    {
        auto const& x = a.outputs[i];
        auto const& y = b.outputs[i];
        if (x.name != y.name || x.bytes.size() != y.bytes.size())
        {
            lines.push_back(cc::format("output {} is '{}' of {} bytes against '{}' of {}", i, y.name, y.bytes.size(),
                                       x.name, x.bytes.size()));
            continue;
        }

        auto const kept = x.is_exact || y.is_exact ? tolerance{} : t;
        auto const count = x.bytes.size() / 4;
        auto const* const xs = reinterpret_cast<u32 const*>(x.bytes.data());
        auto const* const ys = reinterpret_cast<u32 const*>(y.bytes.data());
        auto named = cc::string();
        auto differing = isize(0);
        auto const is_bounded = x.bounds.size() == 2 * count;
        for (auto e = isize(0); e < count; ++e)
        {
            auto const least = is_bounded ? x.bounds[2 * e] : 0.0f;
            auto const most = is_bounded ? x.bounds[2 * e + 1] : 0.0f;
            auto const is_ranged = is_bounded && least == least && most == most;
            if (is_ranged ? within_range(least, most, ys[e], kept, a.ranges_are_hulls) : within(xs[e], ys[e], kept))
                continue;
            if (!is_bounded && both_in_range(ranges, i, x.name, e, xs[e], ys[e], kept))
                continue;
            if (differing < k_named_elements)
                named.appendf("{}[{}] {} against {}{}", named.empty() ? "" : ", ", e, text_of(ys[e], kept),
                              text_of(xs[e], kept), is_ranged ? cc::format(" within [{}, {}]", least, most) : "");
            ++differing;
        }
        if (differing > 0)
            lines.push_back(cc::format("{}: {} of {} elements beyond {}: {}{}", x.name, differing, count,
                                       kept.to_string(), named, differing > k_named_elements ? ", ..." : ""));
    }
    return lines;
}

double sg_diff::tolerance_used(run_result const& a, run_result const& b, tolerance t)
{
    auto most = 0.0;
    for (auto i = isize(0); i < a.outputs.size() && i < b.outputs.size(); ++i)
    {
        auto const& x = a.outputs[i];
        auto const& y = b.outputs[i];
        if (x.is_exact || y.is_exact || x.bytes.size() != y.bytes.size())
            continue;
        auto const count = x.bytes.size() / 4;
        auto const* const xs = reinterpret_cast<u32 const*>(x.bytes.data());
        auto const* const ys = reinterpret_cast<u32 const*>(y.bytes.data());
        auto const is_bounded = x.bounds.size() == 2 * count;
        for (auto e = isize(0); e < count; ++e)
        {
            auto const least = is_bounded ? x.bounds[2 * e] : 0.0f;
            auto const end = is_bounded ? x.bounds[2 * e + 1] : 0.0f;
            auto used = 0.0;
            if (!is_bounded || least != least)
                used = needed(xs[e], ys[e], t.kind);
            else if (least - least != 0.0f || end - end != 0.0f)
                used = 0;
            else
            {
                auto const fy = cc::bit_cast<f32>(ys[e]);
                used = is_nan_bits(ys[e])       ? needed(cc::bit_cast<u32>(least), ys[e], t.kind)
                     : fy >= least && fy <= end ? 0.0
                                                : needed(cc::bit_cast<u32>(fy < least ? least : end), ys[e], t.kind);
            }
            most = used > most ? used : most;
        }
    }
    return most;
}

namespace
{
/// How many elements of a float output a reference keeps: enough to see a vendor's rounding, few enough to review.
constexpr isize k_reference_samples = 64;

[[nodiscard]] u64 fnv1a(cc::span<byte const> bytes)
{
    auto h = u64(0xcbf2'9ce4'8422'2325ull);
    for (auto const b : bytes)
    {
        h ^= u64(b);
        h *= 0x0000'0100'0000'01b3ull;
    }
    return h;
}

/// The elements a reference keeps of an output of `count`: spread evenly and walking through every lane of a vector.
[[nodiscard]] isize sampled_element(isize k, isize count)
{
    return (k * count / k_reference_samples + k) % count;
}

/// The words of a reference line, split at spaces; a name with spaces in it is never one.
[[nodiscard]] cc::vector<cc::string_view> words_of(cc::string_view line)
{
    auto words = cc::vector<cc::string_view>();
    while (!line.empty())
    {
        auto end = isize(0);
        while (end < line.size() && line[end] != ' ')
            ++end;
        if (end > 0)
            words.push_back(line.subview({.offset = 0, .size = end}));
        line = end < line.size() ? line.subview(end + 1) : cc::string_view();
    }
    return words;
}

[[nodiscard]] cc::optional<u64> hex_of(cc::string_view text)
{
    if (!text.starts_with("0x") || text.size() < 3)
        return cc::nullopt;
    auto value = u64(0);
    for (auto const c : text.subview(2))
    {
        auto const digit = c >= '0' && c <= '9' ? int(c - '0') : c >= 'a' && c <= 'f' ? int(c - 'a' + 10) : -1;
        if (digit < 0)
            return cc::nullopt;
        value = value * 16 + u64(digit);
    }
    return value;
}
} // namespace

cc::string sg_diff::reference_name(workload const& w)
{
    auto name = cc::string_view(w.file);
    for (auto i = name.size() - 1; i >= 0; --i)
        if (name[i] == '/')
        {
            name = name.subview(i + 1);
            break;
        }
    if (name.ends_with(".sgl"))
        name = name.subview({.offset = 0, .size = name.size() - 4});
    return cc::format("{}.ref", name);
}

cc::string sg_diff::reference_lines(workload const& w, run_result const& r)
{
    auto text = cc::string();
    for (auto const& o : r.outputs)
    {
        if (w.across.kind == tolerance::measure::exact || o.is_exact)
        {
            text.appendf("{} {} bytes {} fnv1a 0x{:016x}\n", w.entry, o.name, o.bytes.size(), fnv1a(o.bytes));
            continue;
        }
        auto const count = o.bytes.size() / 4;
        auto const* const words = reinterpret_cast<u32 const*>(o.bytes.data());
        for (auto k = isize(0); k < k_reference_samples && count > 0; ++k)
        {
            auto const e = sampled_element(k, count);
            text.appendf("{} {} [{}] 0x{:08x} {}\n", w.entry, o.name, e, words[e], cc::bit_cast<f32>(words[e]));
        }
    }
    return text;
}

cc::vector<cc::string> sg_diff::against_reference(cc::string_view reference,
                                                  workload const& w,
                                                  run_result const& r,
                                                  run_result const* ranges)
{
    auto lines = cc::vector<cc::string>();
    for (auto output = isize(0); output < r.outputs.size(); ++output)
    {
        auto const& o = r.outputs[output];
        auto const count = o.bytes.size() / 4;
        auto const* const words = reinterpret_cast<u32 const*>(o.bytes.data());
        auto seen = isize(0);
        auto differing = isize(0);
        auto named = cc::string();

        auto rest = reference;
        while (!rest.empty())
        {
            auto end = isize(0);
            while (end < rest.size() && rest[end] != '\n')
                ++end;
            auto const line = trimmed(rest.subview({.offset = 0, .size = end}));
            rest = end < rest.size() ? rest.subview(end + 1) : cc::string_view();
            if (line.empty() || line.starts_with("#"))
                continue;
            auto const words_in = words_of(line);
            if (words_in.size() < 4 || words_in[0] != w.entry || words_in[1] != o.name)
                continue;
            ++seen;

            if (words_in[2] == "bytes" && words_in.size() == 6)
            {
                auto const size = cc::from_string<isize>(words_in[3]);
                auto const hash = hex_of(words_in[5]);
                if (!size.has_value() || !hash.has_value())
                    lines.push_back(cc::format("{}: the line '{}' does not parse", o.name, line));
                else if (size.value() != o.bytes.size() || hash.value() != fnv1a(o.bytes))
                    lines.push_back(cc::format("{}: {} bytes of hash 0x{:016x} against the reference's {} of 0x{:016x}",
                                               o.name, o.bytes.size(), fnv1a(o.bytes), size.value(), hash.value()));
                continue;
            }

            auto index_text = words_in[2];
            auto const index
                = index_text.starts_with("[") && index_text.ends_with("]")
                    ? cc::from_string<isize>(index_text.subview({.offset = 1, .size = index_text.size() - 2}))
                    : cc::optional<isize>();
            auto const bits = hex_of(words_in[3]);
            if (!index.has_value() || !bits.has_value() || index.value() < 0 || index.value() >= count)
            {
                lines.push_back(
                    cc::format("{}: the line '{}' does not parse, or names no element of {}", o.name, line, count));
                continue;
            }
            if (within(u32(bits.value()), words[index.value()], w.across)
                || both_in_range(ranges, output, o.name, index.value(), u32(bits.value()), words[index.value()], w.across))
                continue;
            if (differing < k_named_elements)
                named.appendf("{}[{}] {} against {}", named.empty() ? "" : ", ", index.value(),
                              text_of(words[index.value()], w.across), text_of(u32(bits.value()), w.across));
            ++differing;
        }

        if (seen == 0)
            lines.push_back(cc::format("{}: the reference holds nothing of it", o.name));
        if (differing > 0)
            lines.push_back(cc::format("{}: {} of {} referenced elements beyond {}: {}{}", o.name, differing, seen,
                                       w.across.to_string(), named, differing > k_named_elements ? ", ..." : ""));
    }
    return lines;
}
