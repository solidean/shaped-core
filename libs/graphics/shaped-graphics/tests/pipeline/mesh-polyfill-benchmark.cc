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

// What a polyfilled mesh stage costs against the native one, on a device that has both
// (libs/graphics/shaped-graphics/docs/concepts/mesh-polyfill.md).
//
// A frame is one draw of 64 x 64 mesh workgroups into a 1024 x 1024 target, each writing a patch of 64 vertices and
// 98 triangles: about 400 000 triangles, which is the shape of a meshlet renderer's frame.
// The polyfill's number over the native one is what decides whether it is automatic or opt-in.

namespace
{
constexpr int frames_per_iteration = 10;
constexpr int size = 1024;
constexpr int workgroups = 64 * 64;

cc::shared_async<cc::unit> render_frames(sg::context_handle ctx,
                                         sg::raster_pipeline_handle pipeline,
                                         sg::texture_2d const* target)
{
    for (auto frame = 0; frame < frames_per_iteration; ++frame)
    {
        auto cmd = ctx->create_command_list();
        {
            auto pass = cmd->raster.render_to(
                {.color_targets = {target->as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))},
                 .target_set = sg::test::sgl_shaders::meshes::painted::name});
            pass.bind_pipeline(*pipeline);
            pass.draw_mesh_tasks(workgroups);
        }
        (void)ctx->submit_command_list(cc::move(cmd));
        ctx->advance_epoch();
        co_await ctx->epochs_in_flight_completion(2);
    }
    co_await ctx->idle_completion();
}
} // namespace

ASYNC_BENCHMARK("sg mesh polyfill - a frame of meshlets, native against polyfilled")
{
    (void)sg_test::shader_fixtures(); // the library the generated globals resolve through

    for (auto const& factory : sg_test::context_factories())
    {
        auto created = factory.create();
        if (created.has_error())
            continue; // no device for this backend on this machine
        auto const ctx = created.value();
        if (!ctx->supports(sg::feature::mesh_shader) || !ctx->draws_mesh_polyfill())
            continue;

        auto const target = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                                               .width = size,
                                                               .height = size,
                                                               .usage = sg::texture_usage::render_target});
        auto const before = ctx->mesh_polyfill();
        for (auto const use : {sg::polyfill_use::never, sg::polyfill_use::always})
        {
            ctx->set_mesh_polyfill(use);
            auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(sg::test::sgl_shaders::meshes::patched);
            (void)co_await nx::bench::run_async(
                cc::format("{} {}", factory.backend, use == sg::polyfill_use::never ? "native" : "polyfilled"),
                [&] { return render_frames(ctx, pipeline, &target); });
        }
        ctx->set_mesh_polyfill(before);
        co_await ctx->idle_completion();
    }
}
