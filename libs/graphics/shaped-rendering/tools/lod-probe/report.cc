#include "report.hh"

#include <clean-core/string/format.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace lod_probe
{
namespace
{
constexpr float ratios[] = {1, 2, 4, 8, 16}; // the ratios probe.cc's rows have

[[nodiscard]] cc::string signed_lod(float v)
{
    return cc::format("{}{:.3f}", v < 0 ? "-" : "+", tg::abs(v));
}

[[nodiscard]] cc::string row_label(pass const& p)
{
    switch (p.path)
    {
    case path::implicit:
        return cc::format("implicit     sampler bias {}", signed_lod(p.sampler_bias));
    case path::shader_bias:
        return cc::format("shader bias  sample(bias) {}", signed_lod(p.shader_bias));
    case path::gradients:
        return cc::format("gradients    sampler bias {}", signed_lod(p.sampler_bias));
    case path::level:
        return cc::format("level        sampler bias {}", signed_lod(p.sampler_bias));
    }
    return "?";
}

/// Whether two passes differ only in anisotropy, which is what puts them in one row.
[[nodiscard]] bool same_row(pass const& a, pass const& b)
{
    return a.path == b.path && a.sampler_bias == b.sampler_bias && a.shader_bias == b.shader_bias;
}

void append_header(cc::string& out, cc::string_view title)
{
    out += cc::format("{}\n  {:<40}", title, "");
    for (auto const n : anisotropies)
        out += cc::format("{:>8}", cc::format("x{}", n));
    out += "\n";
}

/// One table: a row per pass shape, a column per anisotropy, `cell` for each pass index.
template <class Cell>
void append_table(cc::string& out, cc::string_view title, cc::span<pass_result const> passes, Cell&& cell)
{
    append_header(out, title);
    for (auto i = 0; i < int(passes.size()); ++i)
    {
        auto const& p = passes[i].pass;
        if (p.anisotropy != anisotropies[0])
            continue;
        out += cc::format("  {:<40}", row_label(p));
        for (auto const n : anisotropies)
            for (auto j = 0; j < int(passes.size()); ++j)
                if (passes[j].pass.anisotropy == n && same_row(passes[j].pass, p))
                    out += cc::format("{:>8}", cell(j));
        out += "\n";
    }
    out += "\n";
}

[[nodiscard]] cc::string json_escape(cc::string_view s)
{
    auto out = cc::string();
    for (auto const c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out;
}
} // namespace

cc::string text_report(cc::span<context_result const> contexts, cc::span<tile const> tiles)
{
    auto out = cc::string();
    if (contexts.empty())
        return out;
    out += cc::format("{}\n", contexts[0].adapter);
    for (auto const& c : contexts)
        out += cc::format("  {:<8} driver {}\n", c.backend, c.driver.empty() ? cc::string("unknown") : c.driver);
    out += "\n";

    for (auto const& c : contexts)
    {
        append_table(
            out,
            cc::format("{}: hardware LOD minus specified LOD, read where the sample crosses each half level", c.backend),
            c.passes,
            [&](int j)
            {
                auto const s = summarize(c.passes[j], tiles);
                return s.crossings == 0 ? cc::string("n/a") : signed_lod(s.offset);
            });
        append_table(out,
                     cc::format("{}: blend width, the LOD range one mip transition takes from 10% to 90% (linear = "
                                "0.800)",
                                c.backend),
                     c.passes, [&](int j) { return cc::format("{:.3f}", summarize(c.passes[j], tiles).blend_width); });
    }

    if (contexts.size() < 2)
        return out;

    auto const& a = contexts[0];
    for (auto bi = 1; bi < int(contexts.size()); ++bi)
    {
        auto const& b = contexts[bi];
        auto const delta = [&](int j, float ratio)
        { return summarize(a.passes[j], tiles, ratio).offset - summarize(b.passes[j], tiles, ratio).offset; };

        append_table(out, cc::format("{} minus {}: hardware LOD", a.backend, b.backend), a.passes,
                     [&](int j) { return signed_lod(delta(j, 0)); });

        // The footprint's own anisotropy, on the sampler-bias path at the bias renderers reach for.
        append_header(
            out, cc::format("{} minus {} by footprint ratio, implicit path, sampler bias -1", a.backend, b.backend));
        for (auto const ratio : ratios)
        {
            out += cc::format("  {:<40}", cc::format("footprint {}:1", int(ratio)));
            for (auto const n : anisotropies)
                for (auto j = 0; j < int(a.passes.size()); ++j)
                {
                    auto const& p = a.passes[j].pass;
                    if (p.anisotropy == n && p.path == path::implicit && p.sampler_bias == -1.0f)
                        out += cc::format("{:>8}", signed_lod(delta(j, ratio)));
                }
            out += "\n";
        }
        out += "\n";

        auto worst = 0.0f;
        auto worst_at = 0;
        for (auto j = 0; j < int(a.passes.size()); ++j)
            if (auto const d = delta(j, 0); tg::abs(d) > tg::abs(worst))
            {
                worst = d;
                worst_at = j;
            }
        auto const& wp = a.passes[worst_at].pass;
        out += cc::format("largest gap: {} minus {} = {} LOD, on {}, aniso {}\n", a.backend, b.backend,
                          signed_lod(worst), row_label(wp), wp.anisotropy);
        out += "\n";
    }
    return out;
}

cc::string json_report(cc::span<context_result const> contexts, cc::span<tile const> tiles)
{
    auto out = cc::string("{\n  \"tiles\": [");
    for (auto i = 0; i < int(tiles.size()); ++i)
    {
        auto const& t = tiles[i];
        out += cc::format(R"({}
    {{"minor_lod": {}, "ratio": {}, "angle_degrees": {}, "y_major": {}}})",
                          i == 0 ? "" : ",", t.minor_lod, t.ratio, t.angle_degrees, t.y_major ? "true" : "false");
    }
    out += "\n  ],\n  \"contexts\": [";
    for (auto ci = 0; ci < int(contexts.size()); ++ci)
    {
        auto const& c = contexts[ci];
        out += cc::format(R"({}
    {{"backend": "{}", "adapter": "{}", "driver": "{}", "passes": [)",
                          ci == 0 ? "" : ",", json_escape(c.backend), json_escape(c.adapter), json_escape(c.driver));
        for (auto pi = 0; pi < int(c.passes.size()); ++pi)
        {
            auto const& r = c.passes[pi];
            auto const s = summarize(r, tiles);
            out += cc::format(R"({}
      {{"path": "{}", "anisotropy": {}, "sampler_bias": {}, "shader_bias": {}, "offset": {:.5f}, "blend_width": {:.5f}, "measured": [)",
                              pi == 0 ? "" : ",", to_string(r.pass.path), r.pass.anisotropy, r.pass.sampler_bias,
                              r.pass.shader_bias, s.offset, s.blend_width);
            for (auto i = 0; i < int(r.measured.size()); ++i)
                out += cc::format("{}{:.5f}", i == 0 ? "" : ", ", r.measured[i]);
            out += R"(], "expected": [)";
            for (auto i = 0; i < int(tiles.size()); ++i)
                out += cc::format("{}{:.5f}", i == 0 ? "" : ", ", expected_lod(tiles[i], r.pass));
            out += "]}";
        }
        out += "\n    ]}";
    }
    out += "\n  ]\n}\n";
    return out;
}
} // namespace lod_probe
