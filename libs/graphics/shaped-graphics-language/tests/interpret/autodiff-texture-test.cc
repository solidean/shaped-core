#include "driver-test-support.hh"

// PROTOTYPE (libs/graphics/shaped-graphics-language/docs/spec/incubator/autodiff.md): a texture's @gradient and
// @tangent, run on the interpreter with the textures a driver binds.
// The ramp is one row of four texels, 0, 10, 20 and 30, and the loss twice what a read gives.

using namespace sgl_test;

namespace
{
constexpr cc::string_view k_ramp
    = "binding scene:\n"
      "    @gradient(training.gradients) @tangent(training.tangents) ramp: texture_2d[float]\n"
      "    @no_diff fixed: texture_2d[float]\n"
      "    depth: texture_2d_depth\n"
      "    sampler shadow:\n"
      "        filter = .linear\n"
      "        address = .clamp_edge\n"
      "        compare = .less\n"
      "    sampler linear:\n"
      "        filter = .linear\n"
      "        address = .clamp_edge\n"
      "    sampler nearest:\n"
      "        filter = .nearest\n"
      "        address = .clamp_edge\n"
      "binding training:\n"
      "    gradients: mut buffer[atomic[float]]\n"
      "    tangents: texture_2d[float]\n"
      "\n"
      "fun lit(u: float){scene} -> float => 2.0 * scene.ramp.sample(float2(u, 0.5), scene.linear, level = 0.0)\n"
      "fun stepped(u: float){scene} -> float => 2.0 * scene.ramp.sample(float2(u, 0.5), scene.nearest, level = 0.0)\n"
      "fun loaded(k: int){scene} -> float => 2.0 * scene.ramp.load(int2(k, 0))\n"
      "fun held(u: float){scene} -> float => 2.0 * scene.fixed.sample(float2(u, 0.5), scene.linear, level = 0.0)\n"
      "fun gathered(u: float){scene} -> float => dot(scene.ramp.gather(float2(u, 0.5), scene.linear), float4(1.0, 2.0, "
      "3.0, 4.0))\n"
      "fun shadowed(u: float){scene} -> float => scene.depth.sample_compare(float2(u, 0.5), scene.shadow, reference = "
      "0.5, level = 0.0)\n";

sgl::check::texture_data row(f32 a, f32 b, f32 c, f32 d)
{
    return {.width = 4, .height = 1, .texels = floats(a, b, c, d)};
}

/// The ramp, a @no_diff copy of it, and a tangent texture that moves texel 0 unless `tangents` says otherwise.
sgl::check::driver_bindings ramp_bindings(cc::pinned_data<byte> const& gradients,
                                          sgl::check::texture_data tangents = row(1.0f, 0.0f, 0.0f, 0.0f))
{
    return {.groups = {
                {.name = "scene",
                 .members = {{.name = "ramp", .textures = {row(0.0f, 10.0f, 20.0f, 30.0f)}},
                             {.name = "fixed", .textures = {row(0.0f, 10.0f, 20.0f, 30.0f)}},
                             {.name = "depth", .textures = {row(0.1f, 0.3f, 0.6f, 0.9f)}}}},
                {.name = "training",
                 .members = {{.name = "gradients", .mutable_bytes = gradients},
                             {.name = "tangents", .textures = {cc::move(tangents)}}}},
            }};
}
} // namespace

TEST("sgl autodiff textures - a linear sample adds its adjoint to the texels it weighs, and its coordinate's slope")
{
    // at u = 0.5 the sample is halfway between texels 1 and 2: each takes half of 2, and du is 2 * 4 * (20 - 10)
    auto const gradients = floats(0.0f, 0.0f, 0.0f, 0.0f);
    auto source = cc::string(k_ramp);
    source += "test {scene, training}:\n"
              "    let mut du = 0.0\n"
              "    bwd_diff(lit, 0.5, mut du, 1.0)\n"
              "    du == 80.0\n";
    CHECK(driven_failures_of(source, ramp_bindings(gradients)) == "");
    CHECK(float_at(gradients, 0) == 0.0f);
    CHECK(float_at(gradients, 1) == 1.0f);
    CHECK(float_at(gradients, 2) == 1.0f);
    CHECK(float_at(gradients, 3) == 0.0f);
}

TEST("sgl autodiff textures - going forward a sample changes by its @tangent texture's sample and its coordinate's "
     "slope")
{
    // at u = 0.25 texels 0 and 1 weigh half each, and the tangent texture moves texel 0 alone
    auto const gradients = floats(0.0f, 0.0f, 0.0f, 0.0f);
    auto source = cc::string(k_ramp);
    source += "test {scene, training}:\n"
              "    fwd_diff(lit, 0.25, 0.0) == 1.0\n"
              "    fwd_diff(lit, 0.25, 1.0) == 81.0\n";
    CHECK(driven_failures_of(source, ramp_bindings(gradients)) == "");
}

TEST("sgl autodiff textures - a sample's derivative agrees with a central difference across texels and at the edge")
{
    auto const gradients = floats(0.0f, 0.0f, 0.0f, 0.0f);
    auto source = cc::string(k_ramp);
    source += "fun agrees(u: float){scene, training} -> bool:\n"
              "    let h = 1e-3\n"
              "    let numeric = (lit(u + h) - lit(u - h)) / (2.0 * h)\n"
              "    let mut du = 0.0\n"
              "    bwd_diff(lit, u, mut du, 1.0)\n"
              "    return nearly_equal(du, numeric, within = 1e-2) and nearly_equal(fwd_diff(lit, u, 1.0), numeric, "
              "within = 1e-2)\n"
              "test {scene, training}:\n"
              "    agrees(0.3)\n"
              "    agrees(0.61)\n"
              "    agrees(0.05)\n";
    // the texels hold still, so what changes is the coordinate alone, which a central difference moves
    CHECK(driven_failures_of(source, ramp_bindings(gradients, row(0.0f, 0.0f, 0.0f, 0.0f))) == "");
}

TEST("sgl autodiff textures - a nearest sample and a load add to the one texel they read, and move with no coordinate")
{
    auto const gradients = floats(0.0f, 0.0f, 0.0f, 0.0f);
    auto source = cc::string(k_ramp);
    source += "test {scene, training}:\n"
              "    let mut du = 0.0\n"
              "    bwd_diff(stepped, 0.6, mut du, 1.0)\n"
              "    bwd_diff(loaded, 3, 0.5)\n"
              "    du == 0.0\n";
    CHECK(driven_failures_of(source, ramp_bindings(gradients)) == "");
    CHECK(float_at(gradients, 2) == 2.0f);
    CHECK(float_at(gradients, 3) == 1.0f);
    CHECK(float_at(gradients, 0) == 0.0f);
}

TEST("sgl autodiff textures - a @no_diff texture holds its texels, and its sample still changes with its coordinate")
{
    auto const gradients = floats(0.0f, 0.0f, 0.0f, 0.0f);
    auto source = cc::string(k_ramp);
    source += "test {scene, training}:\n"
              "    let mut du = 0.0\n"
              "    bwd_diff(held, 0.5, mut du, 1.0)\n"
              "    du == 80.0\n";
    CHECK(driven_failures_of(source, ramp_bindings(gradients)) == "");
    CHECK(float_at(gradients, 1) == 0.0f);
}

TEST("sgl autodiff textures - a gather adds each component's adjoint to its texel's one channel, and moves no "
     "coordinate")
{
    // at u = 0.5 the footprint is texels 1 and 2, its second row clamped onto the first: the gather gives (0, 1),
    // (1, 1), (1, 0) and (0, 0) of it, so texel 1 takes weights 1 and 4, and texel 2 weights 2 and 3
    auto const gradients = floats(0.0f, 0.0f, 0.0f, 0.0f);
    auto source = cc::string(k_ramp);
    source += "test {scene, training}:\n"
              "    let mut du = 0.0\n"
              "    bwd_diff(gathered, 0.5, mut du, 1.0)\n"
              "    du == 0.0\n"
              // at u = 0.25 the footprint is texels 0 and 1, and the tangent texture moves texel 0, weighed 1 and 4
              "    fwd_diff(gathered, 0.25, 1.0) == 5.0\n";
    CHECK(driven_failures_of(source, ramp_bindings(gradients)) == "");
    CHECK(float_at(gradients, 0) == 0.0f);
    CHECK(float_at(gradients, 1) == 5.0f);
    CHECK(float_at(gradients, 2) == 5.0f);
    CHECK(float_at(gradients, 3) == 0.0f);
}

TEST("sgl autodiff textures - a comparison sample under a linear filter changes with its coordinate, as a central "
     "difference does")
{
    // texels 1 and 2 hold 0.3 and 0.6, on either side of the reference, so between their centres the result is linear
    auto const gradients = floats(0.0f, 0.0f, 0.0f, 0.0f);
    auto source = cc::string(k_ramp);
    source += "fun compares(u: float){scene} -> bool:\n"
              "    let h = 1e-3\n"
              "    let numeric = (shadowed(u + h) - shadowed(u - h)) / (2.0 * h)\n"
              "    let mut du = 0.0\n"
              "    bwd_diff(shadowed, u, mut du, 1.0)\n"
              "    return numeric != 0.0 and nearly_equal(du, numeric, within = 1e-2)\n"
              "        and nearly_equal(fwd_diff(shadowed, u, 1.0), numeric, within = 1e-2)\n"
              "test {scene, training}:\n"
              "    compares(0.45)\n"
              "    compares(0.55)\n";
    CHECK(driven_failures_of(source, ramp_bindings(gradients)) == "");
}
