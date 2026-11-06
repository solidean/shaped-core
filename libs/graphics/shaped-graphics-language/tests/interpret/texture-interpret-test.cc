#include "driver-test-support.hh"

using namespace sgl_test;

// EVAL-120 to EVAL-126: a test reads the texels its driver binds, and an image store comes back to the driver.

namespace
{
/// A texture's texels as the bytes a driver binds: each scalar 4 bytes, texel after texel.
template <class... F>
sgl::check::texture_data texture(i32 width, i32 height, F... values)
{
    return {.width = width, .height = height, .texels = floats(values...)};
}

/// One `binding scene:` holding `members`, bound to `data` by name.
sgl::check::driver_bindings bound(cc::vector<sgl::check::member_data> data)
{
    return {.groups = {{.name = "scene", .members = cc::move(data)}}};
}
} // namespace

TEST("sgl textures - a load reads the texel a driver binds, at every level, and size says each level's extent")
{
    // level 0 is 2 by 2, level 1 is 1 by 1; a float texel each
    auto t
        = sgl::check::texture_data{.width = 2, .height = 2, .levels = 2, .texels = floats(1.0f, 2.0f, 3.0f, 4.0f, 9.0f)};
    auto const source = "binding scene:\n"
                        "    heights: texture_2d[float]\n"
                        "test {scene}:\n"
                        "    scene.heights.load(int2(1, 0)) == 2.0\n"
                        "    scene.heights.load(int2(0, 1)) == 3.0\n"
                        "    scene.heights.load(int2(0, 0), 1) == 9.0\n"
                        "    scene.heights.size() == int2(2, 2)\n"
                        "    scene.heights.size(1) == int2(1, 1)\n"
                        "    scene.heights.level_count() == 2\n";
    CHECK(driven_failures_of(source, bound({{.name = "heights", .textures = {cc::move(t)}}})) == "");
}

TEST("sgl textures - a load outside the texture has no value, and a texture nothing is bound to reads as empty")
{
    auto const source = "binding scene:\n"
                        "    heights: texture_2d[float]\n"
                        "test {scene}:\n"
                        "    scene.heights.load(int2(2, 0)) == 0.0\n";
    CHECK(driven_failures_of(source, bound({{.name = "heights", .textures = {texture(2, 1, 1.0f, 2.0f)}}}))
              .contains("the texel coordinate 2 is outside 0 ..< 2"));
    // nothing bound: its size is 0, and a load of it has no value
    CHECK(driven_failures_of("binding scene:\n"
                             "    heights: texture_2d[float]\n"
                             "test {scene}:\n"
                             "    scene.heights.size() == int2(0, 0)\n",
                             {})
          == "");
    CHECK(driven_failures_of(source, {}).contains("a texture the run binds no texels to"));
}

TEST("sgl textures - nearest and linear filtering at texel centres and between them, by the sampler's address mode")
{
    // one row of four texels: 0, 10, 20, 30
    auto const row = [] { return texture(4, 1, 0.0f, 10.0f, 20.0f, 30.0f); };
    auto const source = "binding scene:\n"
                        "    ramp: texture_2d[float]\n"
                        "    sampler nearest:\n"
                        "        filter = .nearest\n"
                        "        address = .clamp_edge\n"
                        "    sampler linear:\n"
                        "        filter = .linear\n"
                        "        address = .clamp_edge\n"
                        "    sampler wrapped:\n"
                        "        filter = .linear\n"
                        "        address = .repeat\n"
                        "test {scene}:\n"
                        // texel 1's centre is u = 0.375
                        "    scene.ramp.sample(float2(0.375, 0.5), scene.nearest, level = 0.0) == 10.0\n"
                        "    scene.ramp.sample(float2(0.375, 0.5), scene.linear, level = 0.0) == 10.0\n"
                        // halfway between texels 1 and 2
                        "    scene.ramp.sample(float2(0.5, 0.5), scene.linear, level = 0.0) == 15.0\n"
                        // left of texel 0's centre: clamped to texel 0, and wrapped onto texel 3
                        "    scene.ramp.sample(float2(0.0, 0.5), scene.linear, level = 0.0) == 0.0\n"
                        "    scene.ramp.sample(float2(0.0, 0.5), scene.wrapped, level = 0.0) == 15.0\n"
                        "    scene.ramp.sample(float2(1.125, 0.5), scene.nearest, level = 0.0) == 30.0\n";
    CHECK(driven_failures_of(source, bound({{.name = "ramp", .textures = {row()}}})) == "");
}

TEST("sgl textures - a sample between two levels mixes them under a linear mip filter, and picks one under nearest")
{
    // 2 by 1 at level 0, both 4; 1 by 1 at level 1, 8
    auto t = sgl::check::texture_data{.width = 2, .height = 1, .levels = 2, .texels = floats(4.0f, 4.0f, 8.0f)};
    auto const source = "binding scene:\n"
                        "    pyramid: texture_2d[float]\n"
                        "    sampler trilinear:\n"
                        "        filter = .linear\n"
                        "    sampler stepped:\n"
                        "        filter = .linear\n"
                        "        mip_filter = .nearest\n"
                        "test {scene}:\n"
                        "    scene.pyramid.sample(float2(0.5, 0.5), scene.trilinear, level = 0.25) == 5.0\n"
                        "    scene.pyramid.sample(float2(0.5, 0.5), scene.stepped, level = 0.25) == 4.0\n"
                        "    scene.pyramid.sample(float2(0.5, 0.5), scene.stepped, level = 0.75) == 8.0\n"
                        // a level past the last is the last
                        "    scene.pyramid.sample(float2(0.5, 0.5), scene.trilinear, level = 7.0) == 8.0\n";
    CHECK(driven_failures_of(source, bound({{.name = "pyramid", .textures = {cc::move(t)}}})) == "");
}

TEST("sgl textures - an image store comes back to the driver, and a load after it in the run reads it")
{
    auto texels = floats(0.0f, 0.0f, 0.0f, 0.0f);
    auto const source = "binding scene:\n"
                        "    canvas: mut image_2d[.r32_float]\n"
                        "test {scene}:\n"
                        "    scene.canvas.store(int2(1, 1), 7.5)\n"
                        "    scene.canvas.load(int2(1, 1)) == 7.5\n";
    auto data = cc::vector<sgl::check::member_data>();
    data.push_back({.name = "canvas", .textures = {{.width = 2, .height = 2, .mutable_texels = texels}}});
    CHECK(driven_failures_of(source, bound(cc::move(data))) == "");
    CHECK(float_at(texels, 3) == 7.5f);
    CHECK(float_at(texels, 0) == 0.0f);
}

TEST("sgl textures - a dynamic sampler filters as its driver says, and as the default sampler where it says nothing")
{
    auto const source = "binding scene:\n"
                        "    ramp: texture_2d[float]\n"
                        "    smp: sampler\n"
                        "test {scene}:\n"
                        "    scene.ramp.sample(float2(0.5, 0.5), scene.smp, level = 0.0) == 15.0\n";
    auto const ramp = [] { return texture(4, 1, 0.0f, 10.0f, 20.0f, 30.0f); };
    // the default is linear
    CHECK(driven_failures_of(source, bound({{.name = "ramp", .textures = {ramp()}}})) == "");
    // nearest: u = 0.5 lands in texel 2
    auto nearest = sgl::check::sampler_state{.min_filter = 0, .mag_filter = 0, .mip_filter = 0};
    auto const nearest_source = "binding scene:\n"
                                "    ramp: texture_2d[float]\n"
                                "    smp: sampler\n"
                                "test {scene}:\n"
                                "    scene.ramp.sample(float2(0.5, 0.5), scene.smp, level = 0.0) == 20.0\n";
    CHECK(driven_failures_of(nearest_source,
                             bound({{.name = "ramp", .textures = {ramp()}}, {.name = "smp", .samplers = {nearest}}}))
          == "");
}

TEST("sgl textures - a comparison filters what the comparison gives each texel")
{
    auto const source
        = "binding scene:\n"
          "    shadow: texture_2d_depth\n"
          "    sampler less:\n"
          "        filter = .linear\n"
          "        address = .clamp_edge\n"
          "        compare = .less\n"
          "test {scene}:\n"
          // texels 0.25 and 0.75: a reference of 0.5 is below one of them, halfway between both
          "    scene.shadow.sample_compare(float2(0.5, 0.5), scene.less, reference = 0.5, level = 0.0) == 0.5\n"
          "    scene.shadow.sample_compare(float2(0.25, 0.5), scene.less, reference = 0.5, level = 0.0) "
          "== 0.0\n";
    auto data = cc::vector<sgl::check::member_data>();
    data.push_back({.name = "shadow", .textures = {texture(2, 1, 0.25f, 0.75f)}});
    CHECK(driven_failures_of(source, bound(cc::move(data))) == "");
}

TEST("sgl textures - a gather reads one channel of the four texels a bilinear sample weighs, in the order GPUs give")
{
    // 2 by 2, texel (x, y) = 10 y + x
    auto const source = "binding scene:\n"
                        "    grid: texture_2d[float]\n"
                        "    sampler smp:\n"
                        "        filter = .linear\n"
                        "        address = .clamp_edge\n"
                        "test {scene}:\n"
                        "    scene.grid.gather(float2(0.5, 0.5), scene.smp) == float4(10.0, 11.0, 1.0, 0.0)\n";
    CHECK(driven_failures_of(source, bound({{.name = "grid", .textures = {texture(2, 2, 0.0f, 1.0f, 10.0f, 11.0f)}}}))
          == "");
}

TEST("sgl textures - a cube samples the face its direction's major axis picks")
{
    // six faces of 1 texel, face k holding k
    auto t = sgl::check::texture_data{.width = 1,
                                      .height = 1,
                                      .layers = 6,
                                      .texels = floats(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f)};
    auto const source = "binding scene:\n"
                        "    sky: texture_cube[float]\n"
                        "    sampler smp:\n"
                        "        filter = .nearest\n"
                        "test {scene}:\n"
                        "    scene.sky.sample(float3(1.0, 0.2, 0.1), scene.smp, level = 0.0) == 0.0\n"
                        "    scene.sky.sample(float3(-1.0, 0.2, 0.1), scene.smp, level = 0.0) == 1.0\n"
                        "    scene.sky.sample(float3(0.1, 1.0, 0.2), scene.smp, level = 0.0) == 2.0\n"
                        "    scene.sky.sample(float3(0.1, -1.0, 0.2), scene.smp, level = 0.0) == 3.0\n"
                        "    scene.sky.sample(float3(0.1, 0.2, 1.0), scene.smp, level = 0.0) == 4.0\n"
                        "    scene.sky.sample(float3(0.1, 0.2, -1.0), scene.smp, level = 0.0) == 5.0\n";
    CHECK(driven_failures_of(source, bound({{.name = "sky", .textures = {cc::move(t)}}})) == "");
}

TEST("sgl textures - a test's stated texel grows a texture nothing bound, and stays inside one its driver bound")
{
    // EVAL-127: a stated texel outside the driver's texture has no value, since that texture is the driver's
    auto const source = "binding scene:\n"
                        "    heights: texture_2d[float]\n"
                        "test {scene}:\n"
                        "    scene.heights.test_texel(int2(1, 0), 5.0)\n"
                        "    scene.heights.load(int2(1, 0)) == 5.0\n"
                        "    scene.heights.size() == int2(2, 1)\n";
    CHECK(driven_failures_of(source, bound({{.name = "heights", .textures = {texture(2, 1, 1.0f, 2.0f)}}})) == "");
    CHECK(driven_failures_of(source, {}) == "");
    CHECK(driven_failures_of(source, bound({{.name = "heights", .textures = {texture(1, 1, 1.0f)}}}))
              .contains("outside 0 ..< 1 of the texture the driver bound"));
    CHECK(driven_failures_of("binding scene:\n"
                             "    heights: texture_2d[float]\n"
                             "test {scene}:\n"
                             "    scene.heights.test_texel(int2(-1, 0), 5.0)\n"
                             "    true\n",
                             {})
              .contains("a texel stated at the coordinate -1"));
}

TEST("sgl textures - a texel atomic updates the texel its driver bound, gives what it held, and comes back")
{
    // EVAL-100: an `@atomic` image's texels are memory the run holds, as a buffer's elements are
    u32 const words[] = {0u, 5u};
    auto texels = bytes_of(words);
    auto const source = "require image_atomics\n"
                        "binding scene:\n"
                        "    @atomic depth: mut image_2d[.r32_uint]\n"
                        "test {scene}:\n"
                        "    let before = scene.depth[int2(1, 0)].max(9u)\n"
                        "    before == 5u\n"
                        "    scene.depth[int2(1, 0)].load() == 9u\n"
                        "    scene.depth[int2(0, 0)].add(2u) == 0u\n";
    auto data = cc::vector<sgl::check::member_data>();
    data.push_back({.name = "depth", .textures = {{.width = 2, .height = 1, .mutable_texels = texels}}});
    CHECK(driven_failures_of(source, bound(cc::move(data))) == "");
    CHECK(texels[0] == byte(2));
    CHECK(texels[4] == byte(9));
    // one the run binds no texels to has none to update
    CHECK(driven_failures_of(source, {}).contains("an atomic texel_max of an image the run binds no texels to"));
}

TEST("sgl textures - a static sampler's state is the source's, which a test does not restate")
{
    // EVAL-128
    CHECK(driven_failures_of("binding scene:\n"
                             "    sampler nearest:\n"
                             "        filter = .nearest\n"
                             "test {scene}:\n"
                             "    scene.nearest.test_state(scene.nearest)\n"
                             "    true\n",
                             {})
              .contains("'nearest' is a static sampler, whose state the source sets"));
}

TEST("sgl lanes - a subgroup operation one lane misses has no value, where no check of uniformity sees it")
{
    // EVAL-143: a check's condition is no line the uniformity pass judges, so the run finds the lane that is missing
    CHECK(driven_failures_of("require subgroups\n"
                             "test:\n"
                             "    if test_lane() == 0:\n"
                             "        subgroup_add(1) == 1\n"
                             "    true\n",
                             {})
              .contains("lane 1 of 4 did not reach subgroup_add with the others"));
}

TEST("sgl textures - bytes that are no whole set of texels are refused as invalid bindings")
{
    auto const source = "binding scene:\n"
                        "    heights: texture_2d[float]\n"
                        "test {scene}:\n"
                        "    scene.heights.size() == int2(2, 2)\n";
    CHECK(driven_failures_of(source, bound({{.name = "heights", .textures = {texture(2, 2, 1.0f, 2.0f, 3.0f)}}}))
              .contains("12 bytes of texels, where 4 texels of 1 channels take 16"));
}
