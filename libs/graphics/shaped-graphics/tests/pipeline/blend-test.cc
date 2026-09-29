#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"
#include "rects.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

namespace
{
// Source and destination, each channel a power of two chosen so that every factor below is a different number.
// Red is what the color sweeps read: 0, 1, 1/4, 3/4, 1/16, 15/16, 1/8, 7/8, 1/32 and 31/32 are all distinct.
// So is each of them times 1/4 or 1/16, exactly, in half precision.
constexpr auto source = tg::vec4f(0.25f, 0.5f, 2.0f, 0.125f);
constexpr auto destination = tg::vec4f(0.0625f, 4.0f, 0.5f, 0.03125f);

// The blend constant, red and alpha distinct from every factor above, and so are one minus each: 5/8 and 13/16.
constexpr auto blend_constant = tg::vec4f(0.375f, 0.5f, 0.75f, 0.1875f);

/// The value `f` weighs channel `c` by (3 is alpha), under the blend constant `k`.
float factor_value(sg::blend_factor f, int c, tg::vec4f k)
{
    switch (f)
    {
    case sg::blend_factor::zero:
        return 0;
    case sg::blend_factor::one:
        return 1;
    case sg::blend_factor::src_color:
        return source[c];
    case sg::blend_factor::one_minus_src_color:
        return 1 - source[c];
    case sg::blend_factor::dst_color:
        return destination[c];
    case sg::blend_factor::one_minus_dst_color:
        return 1 - destination[c];
    case sg::blend_factor::src_alpha:
        return source[3];
    case sg::blend_factor::one_minus_src_alpha:
        return 1 - source[3];
    case sg::blend_factor::dst_alpha:
        return destination[3];
    case sg::blend_factor::one_minus_dst_alpha:
        return 1 - destination[3];
    case sg::blend_factor::constant:
        return k[c];
    case sg::blend_factor::one_minus_constant:
        return 1 - k[c];
    }
    return 0;
}

float op_value(sg::blend_op op, float s, float d)
{
    switch (op)
    {
    case sg::blend_op::add:
        return s + d;
    case sg::blend_op::subtract:
        return s - d;
    case sg::blend_op::reverse_subtract:
        return d - s;
    case sg::blend_op::min:
        return s < d ? s : d;
    case sg::blend_op::max:
        return s > d ? s : d;
    }
    return 0;
}

constexpr sg::blend_factor all_factors[] = {
    sg::blend_factor::zero,      sg::blend_factor::one,
    sg::blend_factor::src_color, sg::blend_factor::one_minus_src_color,
    sg::blend_factor::dst_color, sg::blend_factor::one_minus_dst_color,
    sg::blend_factor::src_alpha, sg::blend_factor::one_minus_src_alpha,
    sg::blend_factor::dst_alpha, sg::blend_factor::one_minus_dst_alpha,
    sg::blend_factor::constant,  sg::blend_factor::one_minus_constant,
};

// D3D12 refuses a *_color factor in the alpha equation, so alpha sweeps these.
constexpr sg::blend_factor alpha_factors[] = {
    sg::blend_factor::zero,      sg::blend_factor::one,
    sg::blend_factor::src_alpha, sg::blend_factor::one_minus_src_alpha,
    sg::blend_factor::dst_alpha, sg::blend_factor::one_minus_dst_alpha,
    sg::blend_factor::constant,  sg::blend_factor::one_minus_constant,
};

constexpr sg::blend_op all_ops[]
    = {sg::blend_op::add, sg::blend_op::subtract, sg::blend_op::reverse_subtract, sg::blend_op::min, sg::blend_op::max};

constexpr sg::color_channel channels[]
    = {sg::color_channel::r, sg::color_channel::g, sg::color_channel::b, sg::color_channel::a};

/// The pixel `state` leaves of `source` drawn over `destination` under the blend constant `k`.
tg::vec4f blended(sg::color_target_state const& state, tg::vec4f k)
{
    auto result = source;
    if (state.blend.has_value())
    {
        auto const& b = state.blend.value();
        for (auto c = 0; c < 4; ++c)
        {
            auto const& component = c < 3 ? b.color : b.alpha;
            // min and max ignore the factors on every backend, and WebGPU insists they are one.
            if (component.op == sg::blend_op::min || component.op == sg::blend_op::max)
                result[c] = op_value(component.op, source[c], destination[c]);
            else
                result[c] = op_value(component.op, source[c] * factor_value(component.source, c, k),
                                     destination[c] * factor_value(component.target, c, k));
        }
    }
    for (auto c = 0; c < 4; ++c)
        if (!state.write_mask.has(channels[c]))
            result[c] = destination[c];
    return result;
}

/// One column of the blend test: the target's state, the blend constant its draw sets, and what its pixel must read.
struct blend_case
{
    sg::color_target_state state;
    tg::vec4f constant;
    tg::vec4f expected;
};
} // namespace

ASYNC_INVOCABLE_TEST("sg - each blend factor, op and write-mask channel leaves its own value in a float target",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto cases = cc::vector<blend_case>();
    auto const add_case = [&](sg::color_target_state state, tg::vec4f constant = blend_constant)
    {
        state.format = sg::pixel_format::rgba16_float;
        cases.push_back({.state = state, .constant = constant, .expected = blended(state, constant)});
    };
    auto const opaque = sg::blend_component{.source = sg::blend_factor::one, .target = sg::blend_factor::zero};

    // Every factor as the source's, then as the destination's: the other side is zero, so each column is one product.
    for (auto const f : all_factors)
        add_case({.blend = sg::blend_state{.color = {.source = f, .target = sg::blend_factor::zero}, .alpha = opaque}});
    for (auto const f : all_factors)
        add_case({.blend = sg::blend_state{.color = {.source = sg::blend_factor::zero, .target = f}, .alpha = opaque}});

    // The alpha equation on its own, with color left opaque.
    for (auto const f : alpha_factors)
        add_case({.blend = sg::blend_state{.color = opaque, .alpha = {.source = f, .target = sg::blend_factor::zero}}});
    for (auto const f : alpha_factors)
        add_case({.blend = sg::blend_state{.color = opaque, .alpha = {.source = sg::blend_factor::zero, .target = f}}});

    // Every op, on color and on alpha alike.
    for (auto const op : all_ops)
    {
        auto const both = sg::blend_component{.source = sg::blend_factor::one, .target = sg::blend_factor::one, .op = op};
        add_case({.blend = sg::blend_state{.color = both, .alpha = both}});
    }

    // The blend constant is set per draw: a second constant, set between two draws, is what the second one reads.
    auto const constant_source
        = sg::blend_state{.color = {.source = sg::blend_factor::constant, .target = sg::blend_factor::zero},
                          .alpha = {.source = sg::blend_factor::constant, .target = sg::blend_factor::zero}};
    add_case({.blend = constant_source}, tg::vec4f(0.625f, 0.125f, 0.25f, 0.5f));
    add_case({.blend = constant_source}, blend_constant);

    // Each channel left out of the write mask once, with blending off.
    for (auto const c : channels)
    {
        auto mask = sg::color_write_mask_all;
        mask.remove(c);
        add_case({.write_mask = mask});
    }

    auto pipelines = cc::vector<sg::raster_pipeline_handle>();
    for (auto const& c : cases)
        pipelines.push_back(co_await ctx->cached.acquire_raster_pipeline(
            shaders::rects.floating, {},
            [state = c.state](sg::raster_pipeline_description& d) { d.color_targets[0] = state; }));

    auto const width = int(cases.size());
    auto rects = cc::vector<sg_test::rect>();
    for (auto x = 0; x < width; ++x)
        rects.push_back(sg_test::rect_at(x, 0, x + 1, 1, width, 1, 0.5f, source));
    auto const batch = sg_test::rect_batch(*ctx, rects);

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = width,
                                                          .height = 1,
                                                          .colors = {sg::pixel_format::rgba16_float},
                                                          .target_set = shaders::rect_target::name,
                                                          .clear_color = destination},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             for (auto x = 0; x < width; ++x)
                                                             {
                                                                 scope.bind_pipeline(*pipelines[x]);
                                                                 scope.set_blend_constants(cases[x].constant);
                                                                 batch.draw(scope, x);
                                                             }
                                                         });

    for (auto x = 0; x < width; ++x)
        CHECK(pixels[0].rgba_float(x, 0) == cases[x].expected).context(cc::format("blend case {}", x));
}

ASYNC_INVOCABLE_TEST("sg - a blend constant never set reads as 0", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const state = sg::color_target_state{
        .format = sg::pixel_format::rgba16_float,
        .blend = sg::blend_state{.color = {.source = sg::blend_factor::constant, .target = sg::blend_factor::zero},
                                 .alpha = {.source = sg::blend_factor::constant, .target = sg::blend_factor::zero}},
    };
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(
        shaders::rects.floating, {}, [state](sg::raster_pipeline_description& d) { d.color_targets[0] = state; });

    auto const batch = sg_test::rect_batch(*ctx, {sg_test::rect_at(0, 0, 1, 1, 1, 1, 0.5f, source)});

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 1,
                                                          .height = 1,
                                                          .colors = {sg::pixel_format::rgba16_float},
                                                          .target_set = shaders::rect_target::name,
                                                          .clear_color = destination},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             batch.draw(scope, 0);
                                                         });

    CHECK(pixels[0].rgba_float(0, 0) == tg::vec4f(0, 0, 0, 0));
}
