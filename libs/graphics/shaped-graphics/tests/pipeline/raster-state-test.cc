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
/// `rects.floating`, which draws into an rgba16_float target, with `edit` applied last.
cc::shared_async<sg::raster_pipeline_handle> rect_pipeline(sg::context& ctx,
                                                           cc::unique_function<void(sg::raster_pipeline_description&)> edit)
{
    co_return co_await ctx.cached.acquire_raster_pipeline(shaders::rects.floating, {}, cc::move(edit));
}

auto const white = tg::vec4f(1, 1, 1, 1);

/// Whether the pixel at (x, y) was drawn: its red channel is the white every rect here draws.
bool drawn(sg_test::target_pixels const& target, int x, int y)
{
    return target.rgba_float(x, y)[0] == 1.0f;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - cull_mode and front_face together decide which winding is drawn",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // A column per (cull, front) pair, and two rows: the top rect winds counter-clockwise in clip space, the bottom clockwise.
    struct combination
    {
        sg::cull_mode cull;
        sg::front_face front;
    };
    constexpr combination combinations[] = {
        {.cull = sg::cull_mode::none, .front = sg::front_face::counter_clockwise},
        {.cull = sg::cull_mode::none, .front = sg::front_face::clockwise},
        {.cull = sg::cull_mode::front, .front = sg::front_face::counter_clockwise},
        {.cull = sg::cull_mode::front, .front = sg::front_face::clockwise},
        {.cull = sg::cull_mode::back, .front = sg::front_face::counter_clockwise},
        {.cull = sg::cull_mode::back, .front = sg::front_face::clockwise},
    };
    constexpr int width = 6;
    constexpr int height = 2;

    auto rects = cc::vector<sg_test::rect>();
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            rects.push_back(sg_test::rect_at(x, y, x + 1, y + 1, width, height, 0.5f, white));
    tg::vec2f const clockwise[] = {
        tg::vec2f(0, 0), tg::vec2f(1, 1), tg::vec2f(1, 0), tg::vec2f(0, 0), tg::vec2f(0, 1), tg::vec2f(1, 1),
    };
    auto const counter_clockwise_batch = sg_test::rect_batch(*ctx, rects);
    auto const clockwise_batch = sg_test::rect_batch(*ctx, rects, clockwise);

    auto pipelines = cc::vector<sg::raster_pipeline_handle>();
    for (auto const& c : combinations)
        pipelines.push_back(co_await rect_pipeline(*ctx,
                                                   [c](sg::raster_pipeline_description& d)
                                                   {
                                                       d.rasterization.cull = c.cull;
                                                       d.rasterization.front = c.front;
                                                   }));

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = width,
                                                          .height = height,
                                                          .colors = {sg::pixel_format::rgba16_float},
                                                          .target_set = shaders::rect_target::name},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             for (auto x = 0; x < width; ++x)
                                                             {
                                                                 scope.bind_pipeline(*pipelines[x]);
                                                                 counter_clockwise_batch.draw(scope, x);
                                                                 clockwise_batch.draw(scope, width + x);
                                                             }
                                                         });

    for (auto x = 0; x < width; ++x)
        for (auto y = 0; y < height; ++y)
        {
            auto const& c = combinations[x];
            auto const winds_counter_clockwise = y == 0;
            auto const is_front = winds_counter_clockwise == (c.front == sg::front_face::counter_clockwise);
            auto const culled
                = (c.cull == sg::cull_mode::front && is_front) || (c.cull == sg::cull_mode::back && !is_front);
            CHECK(drawn(pixels[0], x, y) == !culled)
                .context(cc::format("combination {}, the {} rect", x,
                                    winds_counter_clockwise ? "counter-clockwise" : "clockwise"));
        }
}

ASYNC_INVOCABLE_TEST("sg - a wireframe fill leaves a triangle's interior empty", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::wireframe_fill))
        SKIP("this device has no wireframe fill");

    // One rect over the whole target, drawn filled into the left half and in wireframe into the right.
    // (2, 2) is inside the rect's upper-left triangle and two pixels from each of its edges, so only a filled one covers it.
    constexpr int size = 8;
    sg_test::rect const halves[] = {
        sg_test::rect_at(0, 0, size, size, 2 * size, size, 0.5f, white),
        sg_test::rect_at(size, 0, 2 * size, size, 2 * size, size, 0.5f, white),
    };
    auto const batch = sg_test::rect_batch(*ctx, halves);
    auto const solid = co_await rect_pipeline(*ctx, [](sg::raster_pipeline_description&) {});
    auto const wireframe = co_await rect_pipeline(
        *ctx, [](sg::raster_pipeline_description& d) { d.rasterization.fill = sg::fill_mode::wireframe; });

    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 2 * size,
                                                          .height = size,
                                                          .colors = {sg::pixel_format::rgba16_float},
                                                          .target_set = shaders::rect_target::name},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*solid);
                                                             batch.draw(scope, 0);
                                                             scope.bind_pipeline(*wireframe);
                                                             batch.draw(scope, 1);
                                                         });

    CHECK(drawn(pixels[0], 2, 2));
    CHECK(!drawn(pixels[0], size + 2, 2));
    // The wireframe drew something: the diagonal the two triangles share crosses its middle.
    auto any_edge = false;
    for (auto y = 0; y < size; ++y)
        for (auto x = size; x < 2 * size; ++x)
            any_edge = any_edge || drawn(pixels[0], x, y);
    CHECK(any_edge);
}

ASYNC_INVOCABLE_TEST("sg - each primitive_topology assembles one vertex list into its own pixels",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // Four vertices at pixel centres of a 16 × 16 target, in a Z: top-left, top-right, bottom-left, bottom-right.
    // Every probe below lies strictly inside a triangle or on the middle of a line, away from any vertex and from any
    // triangle edge a probe of that topology reads, so no rasterization tie-break decides it.
    constexpr int size = 16;
    tg::vec2f const z[] = {
        sg_test::unit_at_pixel(1.5f, 1.5f, size, size),
        sg_test::unit_at_pixel(14.5f, 1.5f, size, size),
        sg_test::unit_at_pixel(1.5f, 14.5f, size, size),
        sg_test::unit_at_pixel(14.5f, 14.5f, size, size),
    };
    sg_test::rect const whole[] = {sg_test::rect_at(0, 0, size, size, size, size, 0.5f, white)};
    auto const batch = sg_test::rect_batch(*ctx, whole, z);

    // The probes: the upper-left triangle's inside, the lower-right triangle's inside, the top line's middle,
    // the middle of the diagonal from top-right to bottom-left, and the first vertex's own pixel.
    struct probe
    {
        int x;
        int y;
    };
    constexpr probe probes[]
        = {{.x = 4, .y = 4}, {.x = 11, .y = 11}, {.x = 7, .y = 1}, {.x = 8, .y = 7}, {.x = 1, .y = 1}};
    // Per topology and probe: 1 drawn, 0 empty, -1 on an edge or a vertex that topology's rule decides.
    struct expectation
    {
        sg::primitive_topology topology;
        int drawn[5];
    };
    constexpr expectation expectations[] = {
        {.topology = sg::primitive_topology::point_list, .drawn = {0, 0, 0, 0, 1}},
        {.topology = sg::primitive_topology::line_list, .drawn = {0, 0, 1, 0, -1}},
        {.topology = sg::primitive_topology::line_strip, .drawn = {0, 0, 1, 1, -1}},
        {.topology = sg::primitive_topology::triangle_list, .drawn = {1, 0, -1, -1, -1}},
        {.topology = sg::primitive_topology::triangle_strip, .drawn = {1, 1, -1, 1, -1}},
    };

    for (auto const& e : expectations)
    {
        auto const pipeline
            = co_await rect_pipeline(*ctx, [t = e.topology](sg::raster_pipeline_description& d) { d.topology = t; });
        auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                             {.width = size,
                                                              .height = size,
                                                              .colors = {sg::pixel_format::rgba16_float},
                                                              .target_set = shaders::rect_target::name},
                                                             [&](sg::rendering_scope& scope)
                                                             {
                                                                 scope.bind_pipeline(*pipeline);
                                                                 batch.draw(scope, 0);
                                                             });
        for (auto i = 0; i < 5; ++i)
            if (e.drawn[i] >= 0)
                CHECK(drawn(pixels[0], probes[i].x, probes[i].y) == (e.drawn[i] == 1))
                    .context(cc::format("topology {}, probe ({}, {})", int(e.topology), probes[i].x, probes[i].y));
    }
}

ASYNC_INVOCABLE_TEST("sg - a second scope's target_op keeps, clears or discards what the first drew",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // The first scope clears to red and draws white over the left half.
    // The second scope opens with the op under test and draws green over column 3 only.
    // Preserve keeps the white and the red around column 3, clear replaces both by its own color, and discard's
    // contents are undefined, so only column 3 is read after it.
    constexpr int width = 4;
    auto const red = tg::vec4f(1, 0, 0, 1);
    auto const green = tg::vec4f(0, 1, 0, 1);
    auto const blue = tg::vec4f(0, 0, 1, 1);
    sg_test::rect const rects[] = {
        sg_test::rect_at(0, 0, 2, 1, width, 1, 0.5f, white),
        sg_test::rect_at(3, 0, 4, 1, width, 1, 0.5f, green),
    };
    auto const batch = sg_test::rect_batch(*ctx, rects);
    auto const pipeline = co_await rect_pipeline(*ctx, [](sg::raster_pipeline_description&) {});

    for (auto const op : {sg::target_op::preserve, sg::target_op::clear, sg::target_op::discard})
    {
        auto const pixels = co_await sg_test::draw_offscreen_passes(
            *ctx,
            {.width = width,
             .height = 1,
             .colors = {sg::pixel_format::rgba16_float},
             .target_set = shaders::rect_target::name,
             .clear_color = red},
            [&](sg::command_list& cmd, sg_test::offscreen_targets const& targets)
            {
                {
                    auto scope = cmd.raster.render_to(targets.cleared());
                    scope.bind_pipeline(*pipeline);
                    batch.draw(scope, 0);
                }
                auto second = targets.preserved();
                second.color_targets[0].op = op;
                second.color_targets[0].clear_color = blue;
                auto scope = cmd.raster.render_to(second);
                scope.bind_pipeline(*pipeline);
                batch.draw(scope, 1);
            });

        auto const& target = pixels[0];
        CHECK(target.rgba_float(3, 0) == green);
        if (op == sg::target_op::preserve)
        {
            CHECK(target.rgba_float(0, 0) == white);
            CHECK(target.rgba_float(2, 0) == red);
        }
        if (op == sg::target_op::clear)
        {
            CHECK(target.rgba_float(0, 0) == blue);
            CHECK(target.rgba_float(2, 0) == blue);
        }
    }
}

ASYNC_INVOCABLE_TEST("sg - a viewport maps clip space onto its pixels, and a scissor set per draw clips each draw",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    // One rect over all of clip space, drawn three times into an 8 × 4 target, each draw to its own row band:
    //  1. a viewport over columns 4 to 7 of rows 0 and 1, with the full scissor;
    //  2. the full viewport scissored to columns 1 and 2 of row 2;
    //  3. the full viewport scissored to column 6 of row 3, set between two draws of one scope.
    constexpr int width = 8;
    constexpr int height = 4;
    sg_test::rect const whole[] = {sg_test::rect_at(0, 0, width, height, width, height, 0.5f, white)};
    auto const batch = sg_test::rect_batch(*ctx, whole);
    auto const pipeline = co_await rect_pipeline(*ctx, [](sg::raster_pipeline_description&) {});
    auto const box = [](int x0, int y0, int x1, int y1) { return tg::aabb2i(tg::pos2i(x0, y0), tg::pos2i(x1, y1)); };

    auto const pixels = co_await sg_test::draw_offscreen(
        *ctx,
        {.width = width,
         .height = height,
         .colors = {sg::pixel_format::rgba16_float},
         .target_set = shaders::rect_target::name},
        [&](sg::rendering_scope& scope)
        {
            scope.bind_pipeline(*pipeline);
            scope.set_viewport({.offset = tg::pos2f(4, 0), .size = tg::vec2f(4, 2)});
            batch.draw(scope, 0);
            scope.set_viewport({.offset = tg::pos2f(0, 0), .size = tg::vec2f(width, height)});
            scope.set_scissor(box(1, 2, 3, 3));
            batch.draw(scope, 0);
            scope.set_scissor(box(6, 3, 7, 4));
            batch.draw(scope, 0);
        });

    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
        {
            auto const expected = (y < 2 && x >= 4) || (y == 2 && (x == 1 || x == 2)) || (y == 3 && x == 6);
            CHECK(drawn(pixels[0], x, y) == expected).context(cc::format("pixel ({}, {})", x, y));
        }
}
