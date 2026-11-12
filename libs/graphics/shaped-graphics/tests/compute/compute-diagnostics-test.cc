#include "../pipeline/pipeline_harness.hh"
#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-shader-library/binding/binding_groups.hh>

namespace shaders = sg::test::sgl_shaders;

// tests/shaders/sgl/diagnostics.sgl: a package shader with an assert, acquired through the shader library.
// Its text declares sg's debug window, which the library's reflection check must accept as sg's rather than the shader's.

ASYNC_INVOCABLE_TEST("sg - a package shader that asserts is acquired and runs, its debug window accepted as sg's",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::diagnostics::double_checked.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::diagnostics::checked>();

    auto const values = ctx->persistent.create_buffer_from_data(
        cc::vector<int>{1, 2, 3, 4}, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, shaders::diagnostics::checked{.values = values.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(4);
    auto const values_back = cmd->download.data_from_buffer(values);
    ctx->submit_command_list(cc::move(cmd));
    ctx->advance_epoch();

    auto const got = co_await values_back.data();
    REQUIRE(got.size() == 4);
    CHECK(got[0] == 2);
    CHECK(got[1] == 4);
    CHECK(got[2] == 6);
    CHECK(got[3] == 8);

    // the window is read back once its epoch retires, and its reset lands before the test ends
    co_await ctx->idle_completion();
    ctx->process_completed_epochs();
    co_await ctx->idle_completion();
}

ASYNC_INVOCABLE_TEST("sg - a vertex stage's failed assert reaches the log, where the device lets a vertex stage store",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (ctx->backend() == sg::backend_kind::webgpu)
        SKIP("WebGPU's vertex stage writes no storage, so it has no debug window");
    if (!ctx->supports(sg::feature::vertex_pipeline_stores))
        SKIP("this device writes no storage from a vertex stage");
    nx::expect_error("diagnostics.sgl:21: assert failed: vertex 2 of the triangle", nx::exactly(1));

    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::diagnostics::vertex_asserted);
    auto const pixels = co_await sg_test::draw_offscreen(*ctx,
                                                         {.width = 4,
                                                          .height = 4,
                                                          .colors = {sg::pixel_format::rgba8_unorm},
                                                          .target_set = shaders::diagnostics::marked_target::name},
                                                         [&](sg::rendering_scope& scope)
                                                         {
                                                             scope.bind_pipeline(*pipeline);
                                                             scope.draw({.vertex_range = {.offset = 0, .size = 3}});
                                                         });
    CHECK(pixels.colors.size() == 1);

    // the window is asked for once the draw's epoch retires, and decoded once its bytes are back
    ctx->advance_epoch();
    co_await ctx->idle_completion();
    co_await ctx->idle_completion();
    ctx->process_completed_epochs();
    co_await ctx->idle_completion();
}
