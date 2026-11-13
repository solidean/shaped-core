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
        if (def.stage != sg::shader_stage::compute || def.asset == nullptr || *def.asset == nullptr)
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
            result.problems.push_back(cc::format("{}: {}", def.path, problem));
            continue;
        }

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

    // the context's own options, with the route stated rather than the context's preference
    auto options = cc::vector<slib::shader_option>();
    slib::add_context_options(options, *ctx);
    auto kept = cc::vector<slib::shader_option>();
    for (auto& o : options)
        if (o.name != slib::k_native_dxil_option && o.name != slib::k_native_spirv_option)
            kept.push_back(cc::move(o));
    if (r.native)
        kept.push_back({.name = cc::string(r.format == sg::shader_format::dxil ? slib::k_native_dxil_option
                                                                               : slib::k_native_spirv_option),
                        .value = cc::string("true")});

    auto const shader = co_await w->asset->acquire(r.format, kept);
    if (r.native && shader.compiler.name != "sgl")
        co_return run_result{.skipped = cc::format("SGL writes no {} of it yet, so {} compiled it",
                                                   r.format == sg::shader_format::dxil ? "DXIL" : "SPIR-V",
                                                   shader.compiler.name)};

    auto const pipeline
        = co_await ctx->cached.acquire_compute_pipeline({.shader = shader, .layout = w->setup->layout(*ctx)});
    co_return run_result{.outputs = co_await w->setup->run(ctx, pipeline)};
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
