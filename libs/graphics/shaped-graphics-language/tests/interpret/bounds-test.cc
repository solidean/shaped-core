#include "driver-test-support.hh"

using namespace sgl_test;

// A bounding run: what a GPU may give for each float under the entry point's `@float` level (EVAL-6).

namespace
{
f32 const k_nan = cc::bit_cast<f32>(0x7fc0'0000u);

/// What one bounding run of a test stored into `io.output`, and the range of each element.
struct bounded
{
    cc::pinned_data<byte> output;
    cc::pinned_data<f32> bounds;

    [[nodiscard]] f32 value(isize i) const { return float_at(output, i); }
    [[nodiscard]] bool is_exact(isize i) const { return bounds[2 * i] != bounds[2 * i]; }
    [[nodiscard]] f32 least(isize i) const { return bounds[2 * i]; }
    [[nodiscard]] f32 most(isize i) const { return bounds[2 * i + 1]; }
    [[nodiscard]] bool holds(isize i, f32 v) const { return least(i) <= v && v <= most(i); }
};

/// `body` as a test over `io.input`, holding `input`, and `io.output`, of `outputs` floats, run bounding.
/// `prefix` stands before the binding: a `require` or a function the test calls.
bounded run_bounded(cc::string_view body,
                    cc::pinned_data<byte> const& input,
                    isize outputs,
                    sgl::check::float_level floor = sgl::check::float_level::fast,
                    cc::string_view prefix = "")
{
    auto result = bounded{.output = cc::pinned_data<byte>::create_filled(outputs * 4, byte(0)),
                          .bounds = cc::pinned_data<f32>::create_filled(outputs * 2, k_nan)};
    auto const bindings = sgl::check::driver_bindings{
        .groups = {{.name = "io",
                    .members = {{.name = "input", .bytes = input},
                                {.name = "output", .mutable_bytes = result.output, .bounds = result.bounds}}}}};
    auto source = cc::string("require shader_f16\n");
    source += prefix;
    source += "binding io:\n    input: buffer[float]\n    output: mut buffer[float]\n";
    source += "test {io}:\n";
    source += body;
    source += "    true // its stores are what it checks\n";
    CHECK(driven_failures_of(source, bindings, {.bounds_floats = true, .float_floor = floor}) == "");
    return result;
}

/// `a * b - a` of halves, whose product cancels against `a` where `b` is near 1.
constexpr cc::string_view k_cancelling = "    let a = io.input[0] as half\n"
                                         "    let b = io.input[1] as half\n"
                                         "    io.output[0] = (a * b - a) as float\n"
                                         "    io.output[1] = (a * b) as float\n";
} // namespace

TEST("sgl interpret bounds - at fast a half operation may keep a float's precision, and its range holds both")
{
    // half_arithmetic.sgl's element 115, which Intel's Arc computes at a float's precision
    auto const r = run_bounded(k_cancelling, floats(30.984375f, 0.99560546875f), 2);
    CHECK(r.value(0) == -0.140625f);
    CHECK(r.holds(0, -0.140625f));
    // the product kept at a float's precision, then the subtraction too, and that rounded to a half at the end
    CHECK(r.holds(0, 30.984375f * 0.99560546875f - 30.984375f));
    CHECK(r.holds(0, -0.1361083984375f));
    CHECK(r.most(0) - r.least(0) < 0.01f);
    // a product of two halves is exact in a float, which its range reaches
    CHECK(r.holds(1, 30.984375f * 0.99560546875f));
}

TEST("sgl interpret bounds - above fast a half operation rounds, so its range is its value")
{
    auto const r = run_bounded(k_cancelling, floats(30.984375f, 0.99560546875f), 2, sgl::check::float_level::precise);
    CHECK(r.value(0) == -0.140625f);
    CHECK(r.is_exact(0));
    CHECK(r.is_exact(1));
}

TEST("sgl interpret bounds - the level a test reaches is the one a bounding run takes")
{
    auto const prefix = "@float(.precise) fun cancelled(a: half, b: half) -> half:\n    return a * b - a\n";
    auto const r = run_bounded("    io.output[0] = cancelled(io.input[0] as half, io.input[1] as half) as float\n",
                               floats(30.984375f, 0.99560546875f), 1, sgl::check::float_level::fast, prefix);
    CHECK(r.is_exact(0));
}

TEST("sgl interpret bounds - a conversion to a half rounds at every level")
{
    auto const r = run_bounded("    io.output[0] = (io.input[0] as half) as float\n", floats(0.3f), 1);
    CHECK(r.value(0) == 0.300048828125f);
    CHECK(r.is_exact(0));
}

TEST("sgl interpret bounds - a float product may be left unrounded, as a contraction keeps it, above precise alone")
{
    auto const body = "    io.output[0] = io.input[0] * io.input[1]\n"
                      "    io.output[1] = io.input[0] + io.input[1]\n"
                      "    io.output[2] = io.input[2] * io.input[3]\n";
    auto const fast = run_bounded(body, floats(1.1f, 3.3f, 2.0f, 3.0f), 3);
    auto const product = f64(1.1f) * f64(3.3f);
    CHECK(fast.holds(0, 1.1f * 3.3f));
    CHECK(f64(fast.least(0)) <= product);
    CHECK(f64(fast.most(0)) >= product);
    CHECK(fast.least(0) < fast.most(0));
    // a sum is rounded once on every GPU, and its operands are exact here
    CHECK(fast.is_exact(1));
    // a product a float holds exactly has nothing to keep unrounded
    CHECK(fast.is_exact(2));

    auto const precise = run_bounded(body, floats(1.1f, 3.3f, 2.0f, 3.0f), 3, sgl::check::float_level::precise);
    CHECK(precise.is_exact(0));
}

TEST("sgl interpret bounds - a denormal may flush to zero, except at ieee")
{
    auto const body = "    io.output[0] = io.input[0] * io.input[1]\n";
    auto const precise = run_bounded(body, floats(1e-20f, 1e-20f), 1, sgl::check::float_level::precise);
    CHECK(precise.value(0) != 0.0f);
    CHECK(precise.holds(0, 0.0f));
    auto const ieee = run_bounded(body, floats(1e-20f, 1e-20f), 1, sgl::check::float_level::ieee);
    CHECK(ieee.is_exact(0));
}

TEST("sgl interpret bounds - a range carries through every operation after it")
{
    // the product's range, which reaches its exact value, scaled by 4 and offset
    auto const r = run_bounded("    io.output[0] = io.input[0] * io.input[1] * 4.0 + 1.0\n", floats(1.1f, 3.3f), 1);
    auto const exact = f64(1.1f) * f64(3.3f) * 4.0 + 1.0;
    CHECK(r.holds(0, 1.1f * 3.3f * 4.0f + 1.0f));
    CHECK(f64(r.least(0)) <= exact);
    CHECK(f64(r.most(0)) >= exact);
    CHECK(r.least(0) < r.most(0));
    CHECK(r.most(0) - r.least(0) < 1e-5f);
}

TEST("sgl interpret bounds - a branch on a comparison the range leaves open is run both ways, and their stores joined")
{
    // a GPU may keep 1.1 * 3.3 unrounded, which is no float, so whether it is the rounded product is open
    auto const body = "    let p = io.input[0] * io.input[1]\n"
                      "    io.output[0] = p\n"
                      "    if p != io.input[2]:\n"
                      "        io.output[1] = 1.0\n"
                      "    io.output[2] = 2.0\n";
    auto const r = run_bounded(body, floats(1.1f, 3.3f, 1.1f * 3.3f), 3);
    CHECK(!r.is_exact(0));
    CHECK(r.least(0) > -1.0f);
    // the exact way stores nothing there, and the other stores 1
    CHECK(r.holds(1, 0.0f));
    CHECK(r.holds(1, 1.0f));
    CHECK(r.is_exact(2));
}

TEST("sgl interpret bounds - an index the range leaves open makes what follows unbounded")
{
    // the product's range spans dozens of integers once scaled, so which element is stored to is open
    auto const body = "    let p = io.input[0] * io.input[1] * 100000000.0\n"
                      "    io.output[int(p) & 1] = 1.0\n"
                      "    io.output[2] = 2.0\n";
    auto const r = run_bounded(body, floats(1.1f, 3.3f), 3);
    CHECK(r.least(2) < -3.4e38f);
    CHECK(r.most(2) > 3.4e38f);
}

TEST("sgl interpret bounds - a branch the range decides one way leaves what follows bounded")
{
    auto const body = "    let p = io.input[0] * io.input[1]\n"
                      "    if p > 1.0:\n"
                      "        io.output[0] = 1.0\n"
                      "    io.output[1] = 2.0\n";
    auto const r = run_bounded(body, floats(1.1f, 3.3f), 2);
    CHECK(r.value(0) == 1.0f);
    CHECK(r.is_exact(0));
    CHECK(r.is_exact(1));
}

TEST("sgl interpret bounds - a value compared with itself is decided, since a GPU computes it once")
{
    auto const body = "    let p = io.input[0] * io.input[1]\n"
                      "    let q = p\n"
                      "    if p < q or p != q:\n"
                      "        io.output[0] = 1.0\n"
                      "    io.output[1] = 2.0\n";
    auto const r = run_bounded(body, floats(1.1f, 3.3f), 2);
    CHECK(r.value(0) == 0.0f);
    CHECK(r.is_exact(1));
}

TEST("sgl interpret bounds - a range stored into a buffer is read back with it by the next run")
{
    auto output = cc::pinned_data<byte>::create_filled(8, byte(0));
    auto bounds = cc::pinned_data<f32>::create_filled(4, k_nan);
    auto const bindings = sgl::check::driver_bindings{
        .groups = {{.name = "io",
                    .members = {{.name = "input", .bytes = floats(1.1f, 3.3f)},
                                {.name = "output", .mutable_bytes = output, .bounds = bounds}}}}};
    auto const limits = sgl::check::run_limits{.bounds_floats = true};
    auto const header = cc::string("binding io:\n    input: buffer[float]\n    output: mut buffer[float]\n");
    CHECK(driven_failures_of(header + "test {io}:\n    io.output[0] = io.input[0] * io.input[1]\n    true\n", bindings,
                             limits)
          == "");
    auto const least = bounds[0];
    auto const most = bounds[1];
    CHECK(least < most);
    CHECK(driven_failures_of(header + "test {io}:\n    io.output[1] = io.output[0] + 0.0\n    true\n", bindings, limits)
          == "");
    CHECK(bounds[2] == least);
    CHECK(bounds[3] == most);
    // the first element, which the second run did not store to, keeps its range
    CHECK(bounds[0] == least);
}

TEST("sgl interpret bounds - a run that does not bound gives no range")
{
    auto output = cc::pinned_data<byte>::create_filled(4, byte(0));
    auto bounds = cc::pinned_data<f32>::create_filled(2, k_nan);
    auto const bindings = sgl::check::driver_bindings{
        .groups = {{.name = "io",
                    .members = {{.name = "input", .bytes = floats(1.1f, 3.3f)},
                                {.name = "output", .mutable_bytes = output, .bounds = bounds}}}}};
    CHECK(driven_failures_of("binding io:\n    input: buffer[float]\n    output: mut buffer[float]\n"
                             "test {io}:\n    io.output[0] = io.input[0] * io.input[1]\n    true\n",
                             bindings)
          == "");
    CHECK(float_at(output, 0) == 1.1f * 3.3f);
    CHECK(bounds[0] != bounds[0]);
}
