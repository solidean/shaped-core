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

// What a polyfilled tessellation draw costs on a device without tessellation stages (SGL's CHK-527).
//
// A frame draws a grid of quad patches over a 1024 x 1024 target in one draw, at three shapes of about the same triangle count:
// 256 x 256 patches at factor 4, 64 x 64 at factor 16, and 8 x 8 at factor 64.
// The first loads the passes that walk every patch, the last the tessellator that cuts one patch on one thread.

namespace
{
namespace bench = sg::test::sgl_shaders::tessellation_bench;

constexpr int frames_per_iteration = 10;
constexpr int size = 1024;

/// A grid of `grid` x `grid` quad patches over clip space, each's corners counter-clockwise from the bottom left.
cc::vector<tg::vec2f> patches_of(int grid)
{
    auto corners = cc::vector<tg::vec2f>();
    auto const step = 2.0f / float(grid);
    for (auto y = 0; y < grid; ++y)
        for (auto x = 0; x < grid; ++x)
        {
            auto const at = tg::vec2f(-1.0f + float(x) * step, -1.0f + float(y) * step);
            corners.push_back(at);
            corners.push_back(at + tg::vec2f(step, 0));
            corners.push_back(at + tg::vec2f(step, step));
            corners.push_back(at + tg::vec2f(0, step));
        }
    return corners;
}

cc::shared_async<cc::unit> render_frames(sg::context_handle ctx,
                                         sg::raster_pipeline_handle pipeline,
                                         sg::buffer<tg::vec2f> const* corners,
                                         sg::texture_2d const* target)
{
    for (auto frame = 0; frame < frames_per_iteration; ++frame)
    {
        auto cmd = ctx->create_command_list();
        {
            auto pass = cmd->raster.render_to(
                {.color_targets = {target->as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))},
                 .target_set = bench::bench_target::name});
            pass.bind_pipeline(*pipeline);
            pass.bind_vertex_buffer(corners->as_vertex_buffer());
            pass.draw({.vertex_range = {.offset = 0, .size = corners->element_count()}});
        }
        (void)ctx->submit_command_list(cc::move(cmd));
        ctx->advance_epoch();
        co_await ctx->epochs_in_flight_completion(2);
    }
    co_await ctx->idle_completion();
}
} // namespace

ASYNC_BENCHMARK("sg tessellation polyfill - a frame of quad patches, many coarse ones against few fine ones")
{
    (void)sg_test::shader_fixtures(); // the library the generated globals resolve through

    for (auto const& factory : sg_test::context_factories())
    {
        auto created = factory.create();
        if (created.has_error())
            continue; // no device for this backend on this machine
        auto const ctx = created.value();
        if (ctx->supports(sg::feature::tessellation_shader) || !ctx->draws_tessellation_polyfill())
            continue;

        auto const target = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                                               .width = size,
                                                               .height = size,
                                                               .usage = sg::texture_usage::render_target});
        auto const use_before = ctx->tessellation_polyfill();
        ctx->set_tessellation_polyfill(sg::polyfill_use::where_missing);
        struct shape
        {
            int grid;
            int factor;
        };
        for (auto const s : {shape{256, 4}, shape{64, 16}, shape{8, 64}})
        {
            auto const corners
                = ctx->persistent.create_buffer_from_data(patches_of(s.grid), sg::buffer_usage::vertex_buffer);
            auto const pipeline = s.factor == 4  ? co_await ctx->cached.acquire_raster_pipeline(bench::coarse)
                                : s.factor == 16 ? co_await ctx->cached.acquire_raster_pipeline(bench::medium)
                                                 : co_await ctx->cached.acquire_raster_pipeline(bench::fine);
            (void)co_await nx::bench::run_async(
                cc::format("{} {} x {} patches at factor {}", factory.backend, s.grid, s.grid, s.factor),
                [&] { return render_frames(ctx, pipeline, &corners, &target); });
        }
        ctx->set_tessellation_polyfill(use_before);
        co_await ctx->idle_completion();
    }
}
