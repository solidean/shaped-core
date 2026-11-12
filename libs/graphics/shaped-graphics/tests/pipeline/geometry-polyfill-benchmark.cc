#include "../backends/sg_backends.hh"
#include "../shaders/shader_fixtures.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/bench/run_async.hh>
#include <nexus/test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/all.hh>

// What a polyfilled geometry stage costs by each route, on a device without a geometry stage and with mesh shaders
// (SGL's CHK-525): the compute pass ahead of the draw, and the mesh draw.
//
// A frame draws into a 1024 x 1024 target, either in one draw or in 256 draws of a 256th each, since the compute
// route splits the render pass at every draw and the mesh route never does.
// Two stages: one that passes 262 144 small triangles on, and one that grows 262 144 points into quads.

namespace
{
constexpr int frames_per_iteration = 10;
constexpr int size = 1024;
constexpr int grid = 512;

/// A grid of `grid` x `grid` cells over clip space: a point at each cell's centre, or the cell's lower-left triangle.
cc::vector<tg::vec2f> corners_of(bool is_points)
{
    auto corners = cc::vector<tg::vec2f>();
    auto const step = 2.0f / float(grid);
    for (auto y = 0; y < grid; ++y)
        for (auto x = 0; x < grid; ++x)
        {
            auto const at = tg::vec2f(-1.0f + float(x) * step, -1.0f + float(y) * step);
            if (is_points)
                corners.push_back(at + tg::vec2f(step / 2, step / 2));
            else
            {
                corners.push_back(at);
                corners.push_back(at + tg::vec2f(step, 0));
                corners.push_back(at + tg::vec2f(0, step));
            }
        }
    return corners;
}

cc::shared_async<cc::unit> render_frames(sg::context_handle ctx,
                                         sg::raster_pipeline_handle pipeline,
                                         sg::buffer<tg::vec2f> const* corners,
                                         sg::texture_2d const* target,
                                         int draws)
{
    auto const count = corners->element_count();
    for (auto frame = 0; frame < frames_per_iteration; ++frame)
    {
        auto cmd = ctx->create_command_list();
        {
            auto pass = cmd->raster.render_to(
                {.color_targets = {target->as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))},
                 .target_set = sg::test::sgl_shaders::geometry_bench::bench_target::name});
            pass.bind_pipeline(*pipeline);
            pass.bind_vertex_buffer(corners->as_vertex_buffer());
            auto const each = count / draws;
            for (auto d = 0; d < draws; ++d)
                pass.draw({.vertex_range = {.offset = d * each, .size = each}});
        }
        (void)ctx->submit_command_list(cc::move(cmd));
        ctx->advance_epoch();
        co_await ctx->epochs_in_flight_completion(2);
    }
    co_await ctx->idle_completion();
}
} // namespace

ASYNC_BENCHMARK("sg geometry polyfill - a frame through a geometry stage, by the compute route and the mesh route")
{
    (void)sg_test::shader_fixtures(); // the library the generated globals resolve through

    for (auto const& factory : sg_test::context_factories())
    {
        auto created = factory.create();
        if (created.has_error())
            continue; // no device for this backend on this machine
        auto const ctx = created.value();
        if (ctx->supports(sg::feature::geometry_shader) || !ctx->draws_geometry_polyfill()
            || !ctx->supports(sg::feature::mesh_shader) || !ctx->draws_geometry_mesh_route())
            continue;

        auto const target = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                                               .width = size,
                                                               .height = size,
                                                               .usage = sg::texture_usage::render_target});
        auto const triangles = ctx->persistent.create_buffer_from_data(corners_of(false), sg::buffer_usage::vertex_buffer);
        auto const points = ctx->persistent.create_buffer_from_data(corners_of(true), sg::buffer_usage::vertex_buffer);
        auto const use_before = ctx->geometry_polyfill();
        auto const route_before = ctx->geometry_polyfill_route();
        ctx->set_geometry_polyfill(sg::polyfill_use::where_missing);
        for (auto const is_points : {false, true})
            for (auto const draws : {1, 256})
                for (auto const route : {sg::geometry_polyfill_route::compute, sg::geometry_polyfill_route::mesh})
                {
                    ctx->set_geometry_polyfill_route(route);
                    auto const pipeline
                        = is_points ? co_await ctx->cached.acquire_raster_pipeline(
                                          sg::test::sgl_shaders::geometry_bench::billboards)
                                    : co_await ctx->cached.acquire_raster_pipeline(
                                          sg::test::sgl_shaders::geometry_bench::passed_on);
                    auto const* const corners = is_points ? &points : &triangles;
                    (void)co_await nx::bench::run_async(
                        cc::format("{} {}, {} draw{}, {} route", factory.backend, is_points ? "billboards" : "passed on",
                                   draws, draws == 1 ? "" : "s",
                                   route == sg::geometry_polyfill_route::mesh ? "mesh" : "compute"),
                        [&] { return render_frames(ctx, pipeline, corners, &target, draws); });
                }
        ctx->set_geometry_polyfill(use_before);
        ctx->set_geometry_polyfill_route(route_before);
        co_await ctx->idle_completion();
    }
}
