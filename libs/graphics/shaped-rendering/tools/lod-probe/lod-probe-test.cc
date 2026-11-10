#include "probe.hh"

#include <clean-core/common/utility.hh>
#include <nexus/test.hh>

// The specified level the probe measures against: if this is wrong, every offset the tool reports is wrong with it.

namespace
{
bool near(float a, float b)
{
    return a - b < 1e-4f && b - a < 1e-4f;
}
} // namespace

TEST("lod-probe - an isotropic footprint's level is log2 of its derivative length, whatever the anisotropy")
{
    auto const t = lod_probe::tile{.minor_lod = 3.5f, .ratio = 1};
    for (auto const n : {1, 2, 4, 16})
        CHECK(near(lod_probe::specified_lod(t, n), 3.5f));
}

TEST("lod-probe - anisotropic filtering takes the level from the minor axis until the ratio exceeds the sampler's")
{
    auto const t = lod_probe::tile{.minor_lod = 3.0f, .ratio = 8};
    CHECK(near(lod_probe::specified_lod(t, 1), 6.0f));  // off: the major axis
    CHECK(near(lod_probe::specified_lod(t, 2), 5.0f));  // 8:1 footprint, 2:1 allowed
    CHECK(near(lod_probe::specified_lod(t, 8), 3.0f));  // the minor axis
    CHECK(near(lod_probe::specified_lod(t, 16), 3.0f)); // more allowed than needed
}

TEST("lod-probe - rotating a footprint in texture space or swapping its axes keeps its level")
{
    for (auto const angle : {0.0f, 30.0f, 45.0f})
        for (auto const y_major : {false, true})
        {
            auto const t = lod_probe::tile{.minor_lod = 3.25f, .ratio = 4, .angle_degrees = angle, .y_major = y_major};
            CHECK(near(lod_probe::specified_lod(t, 2), 4.25f));
        }
}

TEST("lod-probe - every bias adds to the level, and the explicit level takes the sampler's bias")
{
    auto const t = lod_probe::tile{.minor_lod = 3.0f, .ratio = 2};
    CHECK(near(lod_probe::expected_lod(t, {.anisotropy = 1, .sampler_bias = -1}), 3.0f));
    CHECK(near(lod_probe::expected_lod(t, {.anisotropy = 2, .path = lod_probe::path::shader_bias, .shader_bias = -0.5f}),
               2.5f));
    CHECK(near(lod_probe::expected_lod(t, {.anisotropy = 16, .sampler_bias = -1, .path = lod_probe::path::level}), 2.0f));
}

namespace
{
/// What a sweep reads when the hardware's level is the specified one plus `shift`, through a blend of `width` (linear = 1).
lod_probe::pass_result synthetic(cc::span<lod_probe::tile const> tiles, lod_probe::pass const& p, float shift, float width)
{
    auto r = lod_probe::pass_result{.pass = p};
    for (auto const& t : tiles)
    {
        auto const lod = lod_probe::specified_lod(t, p.anisotropy) + p.sampler_bias + shift;
        auto const m = float(int(lod));
        auto const weight = cc::clamp((lod - m - 0.5f) / width + 0.5f, 0.0f, 1.0f);
        r.measured.push_back(m + weight);
    }
    return r;
}
} // namespace

TEST("lod-probe - a linear blend at the specified level reads no offset and a blend width of 0.8")
{
    auto const tiles = lod_probe::probe_tiles();
    auto const s = lod_probe::summarize(synthetic(tiles, {.anisotropy = 4, .sampler_bias = -1}, 0, 1), tiles);
    CHECK(s.crossings > 0);
    CHECK(s.offset - 0.0f < 0.01f);
    CHECK(0.0f - s.offset < 0.01f);
    CHECK(s.blend_width - 0.8f < 0.02f);
    CHECK(0.8f - s.blend_width < 0.02f);
}

TEST("lod-probe - a sharpened blend shifted by a quarter level reads the shift, not the sharpening")
{
    auto const tiles = lod_probe::probe_tiles();
    auto const s = lod_probe::summarize(synthetic(tiles, {.anisotropy = 16}, 0.25f, 0.4f), tiles);
    CHECK(s.offset - 0.25f < 0.01f);
    CHECK(0.25f - s.offset < 0.01f);
    CHECK(s.blend_width < 0.4f);
}

TEST("lod-probe - no pass of the probe expects a level the pyramid clamps")
{
    auto const tiles = lod_probe::probe_tiles();
    for (auto const& p : lod_probe::probe_passes())
        for (auto const& t : tiles)
            CHECK(!lod_probe::is_clamped(t, p));
}
