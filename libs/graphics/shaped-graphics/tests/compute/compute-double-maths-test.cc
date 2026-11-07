#include "../shaders/shader_fixtures.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_mixed_dxil.hh>
#include <sg_test_mixed_spirv.hh>
#include <sg_test_mixed_text.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// The SGL fixture in tests/shaders/sgl/double_maths.sgl: a double's roots and roundings, which core.sgl writes over its
// bits and `fma` (EMIT-219), against the IEEE values the CPU computes.
// Then sgl_mixed/double_routes.sgl, through DXC and through the context's native route in one context, bit for bit.

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
    // not const: MSVC refuses a constant division by zero
    auto zero = 0.0;
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

/// What `double_steps` gives for each of `values`, three double4 apiece.
template <class Group, class Shader>
cc::shared_async<cc::vector<tg::vec<4, f64>>> stepped(sg::context& ctx, Shader const& shader, cc::vector<f64> const& values)
{
    auto const pipeline = co_await shader.acquire_pipeline(ctx);
    auto const layout = ctx.cached.acquire_binding_group_layout<Group>();
    auto const value_buffer = ctx.persistent.create_buffer_from_data(values, sg::buffer_usage::readonly_buffer);
    auto const results
        = ctx.persistent.create_buffer_from_data(cc::vector<tg::vec<4, f64>>::create_defaulted(3 * values.size()),
                                                 sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto cmd = ctx.create_command_list();
    auto const group = ctx.transient.create_binding_group(
        *cmd, layout, Group{.values = value_buffer.as_readonly_buffer(), .results = results.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(i32(values.size()));
    auto const future = cmd->download.data_from_buffer(results);
    ctx.submit_command_list(cc::move(cmd));
    auto const data = co_await future.data();
    co_return cc::vector<tg::vec<4, f64>>::create_copy_of(cc::span<tg::vec<4, f64> const>(data));
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a double's maths, bits and fma compute alike through DXC and through SGL's binary",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::shader_f64))
        SKIP("this context has no 64-bit floats");
    auto const takes_dxil = ctx->accepts_shader_format(sg::shader_format::dxil);
    if (!takes_dxil && !ctx->accepts_shader_format(sg::shader_format::spirv))
        SKIP("this context takes no native route");
    if (ctx->prefers_native_dxil() || ctx->prefers_native_spirv())
        SKIP("this context takes SGL's binary for every package, so there is no DXC route to compare with");
    // TEMPORARY(bug-sgl-dxil-double-inverse-sqrt-differs-from-dxc-on-amd): the two routes' shapes round apart there.
    if (takes_dxil && ctx->metrics.adapter().vendor_id == 0x1002)
        SKIP("on AMD's dx12 driver a double's inverse_sqrt differs by an ULP between the routes; "
             "issue: bug-sgl-dxil-double-inverse-sqrt-differs-from-dxc-on-amd");

    auto values = double_inputs();
    // (1 + 2^-27)^2 - (1 + 2^-27) is 2^-27 + 2^-54, which a product rounded first loses the last of
    auto const near_one = 1.0 + 7.450580596923828125e-09;
    values.push_back(near_one);

    namespace text = sg::test::mixed_text::double_routes;
    auto const through_dxc = co_await stepped<text::double_routes>(*ctx, text::double_steps, values);
    auto through_native = cc::vector<tg::vec<4, f64>>();
    if (takes_dxil)
    {
        namespace native = sg::test::mixed_dxil::double_routes;
        auto const shader = co_await native::double_steps->acquire(*ctx);
        CHECK(shader.compiler.name == "sgl");
        through_native = co_await stepped<native::double_routes>(*ctx, native::double_steps, values);
    }
    else
    {
        namespace native = sg::test::mixed_spirv::double_routes;
        auto const shader = co_await native::double_steps->acquire(*ctx);
        CHECK(shader.compiler.name == "sgl");
        through_native = co_await stepped<native::double_routes>(*ctx, native::double_steps, values);
    }

    REQUIRE(through_dxc.size() == 3 * values.size());
    REQUIRE(through_native.size() == 3 * values.size());
    // the same SGL, so the same operations: every lane alike to the bit, a NaN alike to any other NaN
    auto const same = [](f64 a, f64 b) { return cc::bit_cast<u64>(a) == cc::bit_cast<u64>(b) || (a != a && b != b); };
    for (auto i = isize(0); i < through_dxc.size(); ++i)
        for (auto lane = 0; lane < 4; ++lane)
            CHECK(same(through_native[i][lane], through_dxc[i][lane]))
                .dump("x", values[i / 3])
                .dump("result", i % 3)
                .dump("lane", lane)
                .dump("native", through_native[i][lane])
                .dump("dxc", through_dxc[i][lane]);

    // a double's bits come back negated by its sign bit alone, a NaN's payload too
    for (auto i = isize(0); i < values.size(); ++i)
        CHECK(cc::bit_cast<u64>(through_native[3 * i + 2][1]) == (cc::bit_cast<u64>(values[i]) ^ (u64(1) << 63)))
            .dump("x", values[i]);

    // EMIT-254: DXIL's double fma rounds once, which vulkan does not promise
    auto const fused = through_native[3 * (values.size() - 1) + 2][0];
    auto const exact = 7.450580596923828125e-09 + 5.5511151231257827e-17;
    if (takes_dxil)
        CHECK(fused == exact).dump("gpu", fused);
    else
        CHECK((fused == exact || fused == 7.450580596923828125e-09)).dump("gpu", fused);
}

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
    // EMIT-256: dx12 takes core.sgl's bodies, and vulkan GLSL.std.450's own, which Vulkan bounds only as a float's
    auto const is_vulkan = ctx->accepts_shader_format(sg::shader_format::spirv);
    // Vulkan's table: `inversesqrt` within 2 ULP of a float, and `sqrt` as 1 / inversesqrt, which a divide's 2.5 more
    // ULP rounds; a float's ULP is at most 2^-23 of the value
    auto const float_ulp = 1.1920928955078125e-07;
    auto const within = [](f64 gpu, f64 cpu, f64 relative) { return tg::abs(gpu - cpu) <= relative * tg::abs(cpu); };
    auto worst_root = i64(0);
    auto worst_inverse = i64(0);
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

        // EVAL-108: on dx12 sqrt within one ULP, its inverse within two; every rounding exact to the bit, a zero's sign kept
        auto const root_of = [](f64 x) { return x < 0.0 ? 0.0 / (x - x) : tg::sqrt(x); };
        // Vulkan bounds a root only in ULP, so a zero, a negative, an infinity and a NaN have no answer stated there
        auto const is_positive_finite = given > 0.0 && given - given == 0.0;
        if (!is_vulkan)
        {
            CHECK(holds([&](f64 x) { return ulps_apart(roots[0], root_of(x)) <= 1; }))
                .dump("x", given)
                .dump("gpu", roots[0])
                .dump("cpu", root_of(given));
            CHECK(holds([&](f64 x) { return ulps_apart(roots[1], 1.0 / root_of(x)) <= 2; }))
                .dump("x", given)
                .dump("gpu", roots[1])
                .dump("cpu", 1.0 / root_of(given));
        }
        else if (is_positive_finite)
        {
            CHECK(holds([&](f64 x) { return within(roots[0], root_of(x), 5 * float_ulp); }))
                .dump("x", given)
                .dump("gpu", roots[0])
                .dump("cpu", root_of(given));
            CHECK(holds([&](f64 x) { return within(roots[1], 1.0 / root_of(x), 2 * float_ulp); }))
                .dump("x", given)
                .dump("gpu", roots[1])
                .dump("cpu", 1.0 / root_of(given));
        }
        if (is_positive_finite && !is_subnormal)
        {
            worst_root = cc::max(worst_root, ulps_apart(roots[0], root_of(given)));
            worst_inverse = cc::max(worst_inverse, ulps_apart(roots[1], 1.0 / root_of(given)));
        }
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
        // Vulkan inherits `fract`'s precision from `x - floor(x)`, a double's no better than a float's there.
        // AMD's driver answers the largest double below 1 where the subtraction rounds to 1.
        auto const fract_of = [](f64 x) { return x - tg::floor(x); };
        if (given - given == 0.0 && !is_vulkan)
            exact(more[1], fract_of, "fract");
        else if (given - given == 0.0)
            CHECK(holds([&](f64 x) { return within(more[1], fract_of(x), float_ulp) || more[1] == fract_of(x); }))
                .dump("x", given)
                .dump("fract", more[1])
                .dump("cpu", fract_of(given));
        auto const length = tg::sqrt(given * given + 1.0);
        if (!is_vulkan)
            CHECK(ulps_apart(more[3], length) <= 1).dump("x", given).dump("gpu", more[3]).dump("cpu", length);
        else if (length - length == 0.0)
            CHECK(within(more[3], length, 5 * float_ulp)).dump("x", given).dump("gpu", more[3]).dump("cpu", length);
    }
    CC_LOG_INFO("{}: a double's sqrt within {} ULP of IEEE's and inverse_sqrt within {}, over positive normal values",
                is_vulkan ? "vulkan" : "dx12", worst_root, worst_inverse);
}
