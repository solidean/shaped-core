#include "differential.hh"

#include "../shaders/shader_fixtures.hh"

#include <clean-core/math/bit.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/from_string.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sg_test_differential.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
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

[[nodiscard]] cc::string text_of(u32 bits, sg_diff::tolerance t)
{
    if (t.kind == sg_diff::tolerance::measure::exact)
        return cc::format("0x{:08x}", bits);
    return cc::format("{} (0x{:08x})", cc::bit_cast<f32>(bits), bits);
}
} // namespace

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

    for (auto const& def : sg::test::differential::package().definitions)
    {
        if (def.asset == nullptr || *def.asset == nullptr)
            continue;
        auto const& asset = *def.asset;
        auto const source = asset->read_source();
        if (!source.has_value())
        {
            result.problems.push_back(cc::format("{}: the library cannot read it", def.path));
            continue;
        }

        auto setup_name = cc::string();
        auto routes = cc::optional<tolerance>();
        auto across = cc::optional<tolerance>();
        auto problem = cc::string();
        auto rest = cc::string_view(source.value());
        while (!rest.empty())
        {
            auto end = isize(0);
            while (end < rest.size() && rest[end] != '\n')
                ++end;
            auto const line = trimmed(rest.subview({.offset = 0, .size = end}));
            rest = end < rest.size() ? rest.subview(end + 1) : cc::string_view();
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
            else
                problem = cc::format("'{}' is no key of a comment block; the keys are setup, routes and across", key);
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

    auto const shader = co_await compiled_on(*ctx, r, *w->asset);
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
    return asset.acquire(r.format, kept);
}

cc::vector<cc::string> sg_diff::differences(run_result const& a, run_result const& b, tolerance t)
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

        auto const count = x.bytes.size() / 4;
        auto const* const xs = reinterpret_cast<u32 const*>(x.bytes.data());
        auto const* const ys = reinterpret_cast<u32 const*>(y.bytes.data());
        auto named = cc::string();
        auto differing = isize(0);
        for (auto e = isize(0); e < count; ++e)
        {
            if (within(xs[e], ys[e], t))
                continue;
            if (differing < k_named_elements)
                named.appendf("{}[{}] {} against {}", named.empty() ? "" : ", ", e, text_of(ys[e], t), text_of(xs[e], t));
            ++differing;
        }
        if (differing > 0)
            lines.push_back(cc::format("{}: {} of {} elements beyond {}: {}{}", x.name, differing, count, t.to_string(),
                                       named, differing > k_named_elements ? ", ..." : ""));
    }
    return lines;
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
        if (w.across.kind == tolerance::measure::exact)
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

cc::vector<cc::string> sg_diff::against_reference(cc::string_view reference, workload const& w, run_result const& r)
{
    auto lines = cc::vector<cc::string>();
    for (auto const& o : r.outputs)
    {
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
            if (within(u32(bits.value()), words[index.value()], w.across))
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
