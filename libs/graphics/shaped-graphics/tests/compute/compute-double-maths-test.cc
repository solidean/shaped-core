#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// The SGL fixture in tests/shaders/sgl/double_maths.sgl: a double's roots and roundings, which the text writes as HLSL
// of its own (CHK-458), against the IEEE values the CPU computes.

namespace
{
/// How many doubles lie between `a` and `b`, 0 for the same value; two NaNs are 0 apart, and a NaN beside a number far.
i64 ulps_apart(f64 a, f64 b)
{
    if (a != a || b != b)
        return a != a && b != b ? 0 : i64(1) << 62;
    if (a == b)
        return 0;
    // the bits as a line on which the doubles are in order, negative ones mirrored below zero
    auto const ordered = [](f64 x)
    {
        auto const bits = cc::bit_cast<i64>(x);
        return bits < 0 ? (i64(1) << 63) - bits : bits;
    };
    auto const d = ordered(a) - ordered(b);
    return d < 0 ? -d : d;
}

/// A zero keeps the sign of `x`, as the GPU's does.
f64 round_to_even(f64 x)
{
    auto const below = tg::floor(x);
    auto const rest = x - below;
    auto const rounded = rest > 0.5 || (rest == 0.5 && tg::floor(below / 2.0) * 2.0 != below) ? below + 1.0 : below;
    return rounded == 0.0 && x < 0.0 ? -0.0 : rounded;
}

f64 truncated(f64 x)
{
    return x < 0.0 ? -tg::floor(-x) : tg::floor(x);
}

/// Values that reach every branch of the HLSL: zeros, subnormals, both ends of the exponents, ties, integers past
/// 2^52, specials and a spread of mantissas over the whole range.
cc::vector<f64> double_inputs()
{
    auto const zero = 0.0;
    auto values = cc::vector<f64>{0.0,
                                  -0.0,
                                  1.0,
                                  2.0,
                                  0.5,
                                  2.5,
                                  -2.5,
                                  3.5,
                                  -0.25,
                                  -0.75,
                                  1.0e300,
                                  1.0e-300,
                                  4.9406564584124654e-324,
                                  1.0e-310,
                                  4503599627370497.0,
                                  4503599627370495.5,
                                  -1234.5678,
                                  16777216.25,
                                  1.7976931348623157e308,
                                  -1.0,
                                  1.0 / zero,
                                  -1.0 / zero,
                                  zero / zero};
    for (auto i = 0; values.size() < 256; ++i)
    {
        auto const mantissa = 1.0 + f64((i * 37) % 256) / 256.0 + f64(i) * 1.0e-9;
        auto exponent = (i * 151) % 2040 - 1060;
        auto scale = 1.0;
        for (; exponent > 0; --exponent)
            scale *= 2.0;
        for (; exponent < 0; ++exponent)
            scale *= 0.5;
        values.push_back((i % 3 == 0 ? -mantissa : mantissa) * scale);
    }
    return values;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - an SGL compute shader's double sqrt and rounding are IEEE's, to an ULP",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::shader_f64))
        SKIP("this context has no 64-bit floats");

    auto const pipeline = co_await shaders::double_maths::double_functions.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::double_maths::double_maths>();

    auto const values = double_inputs();
    auto const count = values.size();
    auto const value_buffer = ctx->persistent.create_buffer_from_data(values, sg::buffer_usage::readonly_buffer);
    auto const results
        = ctx->persistent.create_buffer_from_data(cc::vector<tg::vec<4, f64>>::create_defaulted(2 * count),
                                                  sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(*cmd, layout,
                                                           shaders::double_maths::double_maths{
                                                               .values = value_buffer.as_readonly_buffer(),
                                                               .results = results.as_readwrite_buffer(),
                                                           });
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(i32(count));
    auto const future = cmd->download.data_from_buffer(results);
    ctx->submit_command_list(cc::move(cmd));

    auto const data = co_await future.data();
    REQUIRE(data.size() == 2 * count);
    for (auto i = isize(0); i < count; ++i)
    {
        auto const given = values[i];
        auto const roots = data[2 * i];
        auto const more = data[2 * i + 1];
        // B12: the fixture runs at `@float(.fast)`, where a subnormal may be read as the zero of its sign, so either
        // answer stands for one
        auto const is_subnormal = given != 0.0 && given < 2.2250738585072014e-308 && given > -2.2250738585072014e-308;
        auto const flushed = given < 0.0 ? -0.0 : 0.0;
        auto const holds = [&](auto&& for_value) { return for_value(given) || (is_subnormal && for_value(flushed)); };

        // EVAL-108: sqrt within one ULP, its inverse within two, and every rounding exact to the bit, a zero's sign kept
        auto const root_of = [](f64 x) { return x < 0.0 ? 0.0 / (x - x) : tg::sqrt(x); };
        CHECK(holds([&](f64 x) { return ulps_apart(roots[0], root_of(x)) <= 1; }))
            .dump("x", given)
            .dump("gpu", roots[0])
            .dump("cpu", root_of(given));
        CHECK(holds([&](f64 x) { return ulps_apart(roots[1], 1.0 / root_of(x)) <= 2; }))
            .dump("x", given)
            .dump("gpu", roots[1])
            .dump("cpu", 1.0 / root_of(given));
        auto const exact = [&](f64 gpu, f64 (*cpu)(f64), char const* what)
        {
            auto const is_same = holds(
                [&](f64 x)
                { return cc::bit_cast<u64>(gpu) == cc::bit_cast<u64>(cpu(x)) || (gpu != gpu && cpu(x) != cpu(x)); });
            CHECK(is_same).dump("x", given).dump(what, gpu).dump("cpu", cpu(given));
        };
        exact(roots[2], [](f64 x) { return tg::floor(x); }, "floor");
        exact(roots[3], [](f64 x) { return tg::ceil(x); }, "ceil");
        exact(more[0], round_to_even, "round");
        exact(more[2], truncated, "trunc");
        // `.fast` keeps no NaN or infinity through arithmetic, and `fract` is arithmetic on its argument
        if (given - given == 0.0)
            exact(more[1], [](f64 x) { return x - tg::floor(x); }, "fract");
        auto const length = tg::sqrt(given * given + 1.0);
        CHECK(ulps_apart(more[3], length) <= 1).dump("x", given).dump("gpu", more[3]).dump("cpu", length);
    }
}
