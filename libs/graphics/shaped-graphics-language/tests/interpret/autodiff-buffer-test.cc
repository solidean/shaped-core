#include "driver-test-support.hh"

// PROTOTYPE (libs/graphics/shaped-graphics-language/docs/spec/incubator/autodiff.md): a buffer's @gradient and
// @tangent, run on the interpreter with the buffers a driver binds.

using namespace sgl_test;

namespace
{
constexpr cc::string_view k_fit
    = "binding params:\n"
      "    @gradient(training.gradients) @tangent(training.tangents) weights: buffer[float]\n"
      "binding training:\n"
      "    gradients: mut buffer[atomic[float]]\n"
      "    tangents: buffer[float]\n"
      "\n"
      "// sum over i of (w_i x - 1)^2, each weight read in a loop of its own index\n"
      "fun loss(x: float){params} -> float:\n"
      "    let mut total = 0.0\n"
      "    for i in 0 ..< 4:\n"
      "        let d = params.weights[i] * x - 1.0\n"
      "        total += d * d\n"
      "    return total\n";

sgl::check::driver_bindings fit_bindings(cc::pinned_data<byte> const& gradients, cc::pinned_data<byte const> tangents)
{
    return {.groups = {
                {.name = "params", .members = {{.name = "weights", .bytes = floats(1.0f, 2.0f, 3.0f, 4.0f)}}},
                {.name = "training",
                 .members = {{.name = "gradients", .mutable_bytes = gradients}, {.name = "tangents", .bytes = tangents}}},
            }};
}
} // namespace

TEST("sgl autodiff buffers - a read's adjoint is added to its @gradient at its index, onto what it held")
{
    // at x = 0.5: d_i = w_i / 2 - 1 is -0.5, 0, 0.5, 1; the gradient of w_i is 2 d_i x = d_i, of x the sum of 2 d_i w_i
    auto const gradients = floats(1.0f, 1.0f, 1.0f, 1.0f);
    auto source = cc::string(k_fit);
    source += "test {params, training}:\n"
              "    let mut dx = 0.0\n"
              "    bwd_diff(loss, 0.5, mut dx, 1.0)\n"
              "    dx == 10.0\n";
    CHECK(driven_failures_of(source, fit_bindings(gradients, floats(0.0f, 0.0f, 0.0f, 0.0f))) == "");
    CHECK(float_at(gradients, 0) == 0.5f);
    CHECK(float_at(gradients, 1) == 1.0f);
    CHECK(float_at(gradients, 2) == 1.5f);
    CHECK(float_at(gradients, 3) == 2.0f);
}

TEST("sgl autodiff buffers - going forward a read changes by its @tangent's element, beside its arguments' tangents")
{
    // the weights move by (1, 0, 0, 0) and x by 1: d_0 x = -0.25 from the weight, and 10 from x
    auto const gradients = floats(0.0f, 0.0f, 0.0f, 0.0f);
    auto source = cc::string(k_fit);
    source += "test {params, training}:\n"
              "    fwd_diff(loss, 0.5, 0.0) == -0.5\n"
              "    fwd_diff(loss, 0.5, 1.0) == 9.5\n";
    CHECK(driven_failures_of(source, fit_bindings(gradients, floats(1.0f, 0.0f, 0.0f, 0.0f))) == "");
    // a forward derivative writes no gradient
    CHECK(float_at(gradients, 0) == 0.0f);
}

TEST("sgl autodiff buffers - two reads of one element in one statement each add their share")
{
    auto const gradients = floats(0.0f, 0.0f, 0.0f, 0.0f);
    auto source = cc::string("binding params:\n"
                             "    @gradient(training.gradients) weights: buffer[float]\n"
                             "binding training:\n"
                             "    gradients: mut buffer[atomic[float]]\n"
                             "    tangents: buffer[float]\n"
                             "\n"
                             "fun squared(k: int){params} => params.weights[k] * params.weights[k]\n"
                             "test {params, training}:\n"
                             "    bwd_diff(squared, 2, 1.0)\n"
                             "    bwd_diff(squared, 2, 0.5)\n"
                             "    true // what it checks is the gradient, which the driver reads back\n");
    CHECK(driven_failures_of(source, fit_bindings(gradients, floats(0.0f))) == "");
    // 2 w_2 for each, the second scaled by its 0.5
    CHECK(float_at(gradients, 2) == 9.0f);
    CHECK(float_at(gradients, 0) == 0.0f);
}
