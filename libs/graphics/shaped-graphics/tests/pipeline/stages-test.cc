#include "../shaders/shader_fixtures.hh"
#include "pipeline_harness.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// stages.sgl's geometry and tessellation stages, drawn where the device has them.
// Every probe is a pixel centre well inside or well outside each triangle, so no rasterization tie-break decides it.

namespace
{
constexpr int size = 16;

/// The clip-space centre of pixel (x, y) of the `size` × `size` target.
tg::vec2f centre_of(int x, int y)
{
    return tg::vec2f(-1.0f + (2.0f * float(x) + 1.0f) / float(size), 1.0f - (2.0f * float(y) + 1.0f) / float(size));
}

/// Where `p` sits against triangle `a, b, c`: 1 inside, -1 outside, 0 within a pixel's tenth of an edge.
int side_of(tg::vec2f p, tg::vec2f a, tg::vec2f b, tg::vec2f c)
{
    auto const edge = [&](tg::vec2f from, tg::vec2f to)
    {
        auto const d = to - from;
        return (d[0] * (p[1] - from[1]) - d[1] * (p[0] - from[0])) / d.length();
    };
    auto const e0 = edge(a, b);
    auto const e1 = edge(b, c);
    auto const e2 = edge(c, a);
    auto const margin = 0.2f / float(size);
    auto const all_positive = e0 > margin && e1 > margin && e2 > margin;
    auto const all_negative = e0 < -margin && e1 < -margin && e2 < -margin;
    if (all_positive || all_negative)
        return 1;
    auto const near = [&](float e) { return e > -margin && e < margin; };
    if (near(e0) || near(e1) || near(e2))
        return 0;
    return -1;
}

sg_test::offscreen stage_target()
{
    return {.width = size,
            .height = size,
            .colors = {sg::pixel_format::rgba32_float},
            .target_set = shaders::stage_target::name,
            .clear_color = tg::vec4f(-1, -1, -1, -1)};
}

bool drawn(sg_test::offscreen_pixels const& pixels, int x, int y)
{
    return pixels[0].rgba_float(x, y)[3] == 1.0f;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a tessellated triangle patch covers its triangle, reports its domain, and winds as its "
                     "control stage says",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::tessellation_shader))
        SKIP("this device has no tessellation stages");

    // The patch is the lower-left half of the target, its corners counter-clockwise in clip space.
    // The evaluation stage writes its domain location as the color, which is each pixel centre's barycentric
    // coordinates against the three corners.
    auto const p0 = tg::vec2f(-1, -1);
    auto const p1 = tg::vec2f(1, -1);
    auto const p2 = tg::vec2f(-1, 1);
    shaders::stage_corner const corners[] = {{.position = p0}, {.position = p1}, {.position = p2}};
    auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);

    auto const draw_with = [&](auto const& declared) -> cc::shared_async<sg_test::offscreen_pixels>
    {
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(declared);
        co_return co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                   [&](sg::rendering_scope& scope)
                                                   {
                                                       scope.bind_pipeline(*pipeline);
                                                       scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                       scope.draw({.vertex_range = {.offset = 0, .size = 3}});
                                                   });
    };

    // Culling back faces with counter-clockwise fronts, the patch whose control stage says counter-clockwise is drawn
    // whole and the clockwise one culled: the winding a control stage names is the patch's, as the evaluation below
    // weighs its corners (CHK-304).
    auto const ccw = co_await draw_with(shaders::stages.tessellated_ccw);
    auto const cw = co_await draw_with(shaders::stages.tessellated_cw);
    auto const culled = co_await draw_with(shaders::stages.tessellated_culled);

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const c = centre_of(x, y);
            auto const side = side_of(c, p0, p1, p2);
            if (side == 0)
                continue;
            auto const where = cc::format("pixel ({}, {})", x, y);
            CHECK(drawn(ccw, x, y) == (side > 0)).context(where);
            CHECK(!drawn(cw, x, y)).context(where);
            CHECK(!drawn(culled, x, y)).context(where); // an edge factor of zero culls the patch
            if (side > 0)
            {
                auto const v = (c[0] + 1.0f) / 2.0f;
                auto const w = (c[1] + 1.0f) / 2.0f;
                auto const got = ccw[0].rgba_float(x, y);
                auto const near = [](float a, float b) { return a - b < 0.001f && b - a < 0.001f; };
                CHECK((near(got[0], 1.0f - v - w) && near(got[1], v) && near(got[2], w)))
                    .context(cc::format("{}: domain ({}, {}, {})", where, got[0], got[1], got[2]));
            }
        }
}

ASYNC_INVOCABLE_TEST("sg - a tessellated quad patch covers its quad, reports its domain, and winds as its control "
                     "stage says",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::tessellation_shader))
        SKIP("this device has no tessellation stages");

    // The patch is the middle half of the target, its corners counter-clockwise from the bottom left, and the
    // evaluation stage blends them by `uv` along the bottom edge and then upwards.
    shaders::stage_corner const corners[] = {{.position = tg::vec2f(-0.5f, -0.5f)},
                                             {.position = tg::vec2f(0.5f, -0.5f)},
                                             {.position = tg::vec2f(0.5f, 0.5f)},
                                             {.position = tg::vec2f(-0.5f, 0.5f)}};
    auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
    auto const draw_with = [&](auto const& declared) -> cc::shared_async<sg_test::offscreen_pixels>
    {
        auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(declared);
        co_return co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                   [&](sg::rendering_scope& scope)
                                                   {
                                                       scope.bind_pipeline(*pipeline);
                                                       scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                       scope.draw({.vertex_range = {.offset = 0, .size = 4}});
                                                   });
    };
    auto const ccw = co_await draw_with(shaders::stages.quad_ccw);
    auto const cw = co_await draw_with(shaders::stages.quad_cw);

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const c = centre_of(x, y);
            // pixel centres sit a quarter pixel off the patch's edges, so each is clearly in or out
            auto const inside = c[0] > -0.5f && c[0] < 0.5f && c[1] > -0.5f && c[1] < 0.5f;
            auto const where = cc::format("pixel ({}, {})", x, y);
            CHECK(drawn(ccw, x, y) == inside).context(where);
            CHECK(!drawn(cw, x, y)).context(where);
            if (inside)
            {
                auto const got = ccw[0].rgba_float(x, y);
                auto const near = [](float a, float b) { return a - b < 0.001f && b - a < 0.001f; };
                CHECK((near(got[0], c[0] + 0.5f) && near(got[1], c[1] + 0.5f)))
                    .context(cc::format("{}: domain ({}, {})", where, got[0], got[1]));
            }
        }
}

ASYNC_INVOCABLE_TEST("sg - a geometry stage's ended strips draw each copy and nothing between them",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::geometry_shader))
        SKIP("this device has no geometry stage");

    // One triangle in the left half, which the geometry stage emits again one unit to the right.
    // Without the strip ended between them, the strip's middle triangle would bridge the two across the bottom.
    auto const a = tg::vec2f(-1, -1);
    auto const b = tg::vec2f(0, -1);
    auto const c = tg::vec2f(-1, 1);
    auto const shift = tg::vec2f(1, 0);
    shaders::stage_corner const corners[] = {{.position = a}, {.position = b}, {.position = c}};
    auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages.doubled);

    auto const pixels = co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 3}});
                                                         });

    auto bridged = 0;
    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const p = centre_of(x, y);
            auto const first = side_of(p, a, b, c);
            auto const second = side_of(p, a + shift, b + shift, c + shift);
            auto const bridge = side_of(p, c, a + shift, b + shift);
            if (first == 0 || second == 0 || bridge == 0)
                continue;
            auto const inside = first > 0 || second > 0;
            bridged += !inside && bridge > 0 ? 1 : 0;
            CHECK(drawn(pixels, x, y) == inside).context(cc::format("pixel ({}, {})", x, y));
        }
    CHECK(bridged > 0); // the probes include pixels only a bridging triangle would reach
}

ASYNC_INVOCABLE_TEST("sg - a geometry stage reads each triangle's primitive id and passes it on flat",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::geometry_shader))
        SKIP("this device has no geometry stage");

    // Two triangles over the whole target: the lower-right is primitive 0 and the upper-left primitive 1.
    auto const bl = tg::vec2f(-1, -1);
    auto const br = tg::vec2f(1, -1);
    auto const tr = tg::vec2f(1, 1);
    auto const tl = tg::vec2f(-1, 1);
    shaders::stage_corner const corners[]
        = {{.position = bl}, {.position = br}, {.position = tr}, {.position = bl}, {.position = tr}, {.position = tl}};
    auto const buffer = ctx->persistent.create_buffer_from_data(corners, sg::buffer_usage::vertex_buffer);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::stages.primitive_ids);

    auto const pixels = co_await sg_test::draw_offscreen(*ctx, stage_target(),
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.bind_vertex_buffer(buffer.as_vertex_buffer());
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 6}});
                                                         });

    for (auto y = 0; y < size; ++y)
        for (auto x = 0; x < size; ++x)
        {
            auto const p = centre_of(x, y);
            auto const lower = side_of(p, bl, br, tr);
            auto const upper = side_of(p, bl, tr, tl);
            if (lower == 0 || upper == 0)
                continue;
            CHECK(pixels[0].rgba_float(x, y)[0] == (lower > 0 ? 0.0f : 1.0f)).context(cc::format("pixel ({}, {})", x, y));
        }
}

ASYNC_INVOCABLE_TEST("sg - a pipeline with a geometry stage the device lacks is refused naming geometry_shader",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (ctx->supports(sg::feature::geometry_shader))
        SKIP("this device has a geometry stage, so nothing is refused");

    auto const built = ctx->cached.acquire_raster_pipeline(shaders::stages.doubled);
    co_await cc::async_settled(built);
    REQUIRE(built->has_error());
    CHECK(built->try_error()->underlying().to_string().contains("geometry_shader"));
}

ASYNC_INVOCABLE_TEST("sg - a pipeline with tessellation stages the device lacks is refused naming tessellation_shader",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (ctx->supports(sg::feature::tessellation_shader))
        SKIP("this device has tessellation stages, so nothing is refused");

    auto const built = ctx->cached.acquire_raster_pipeline(shaders::stages.tessellated_ccw);
    co_await cc::async_settled(built);
    REQUIRE(built->has_error());
    CHECK(built->try_error()->underlying().to_string().contains("tessellation_shader"));
}

ASYNC_INVOCABLE_TEST("sg - a wireframe pipeline on a device without wireframe fill is refused naming wireframe_fill",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (ctx->supports(sg::feature::wireframe_fill))
        SKIP("this device has wireframe fill, so nothing is refused");

    auto const built
        = ctx->cached.acquire_raster_pipeline(shaders::rects.floating, {}, [](sg::raster_pipeline_description& d)
                                              { d.rasterization.fill = sg::fill_mode::wireframe; });
    co_await cc::async_settled(built);
    REQUIRE(built->has_error());
    CHECK(built->try_error()->underlying().to_string().contains("wireframe_fill"));
}
