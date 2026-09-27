#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-shader-library/shader_asset.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// What a usage pattern costs in barriers, now that a dispatch declares what its pipeline's code does rather than what
// its bindings allow — see libs/graphics/shaped-graphics/docs/concepts/barriers.md, "Access follows the footprint".
// Every binding of footprint.sgl is writable, so each zero here was a barrier before.
//
// A barrier a list needs on ENTRY is counted too, so each test settles its inputs with a list of its own before it takes
// its first reading, and writes only into buffers nothing has touched yet, where the first write is free.

namespace
{
constexpr auto k_count = 256;

sg::buffer<float> make_buffer(sg::context& ctx)
{
    return ctx.persistent.create_buffer<float>(k_count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_dst
                                                            | sg::buffer_usage::copy_src
                                                            | sg::buffer_usage::readonly_buffer);
}

cc::vector<float> ramp()
{
    auto values = cc::vector<float>::create_defaulted(k_count);
    for (auto i = 0; i < k_count; ++i)
        values[i] = float(i);
    return values;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - two dispatches that only load a mut buffer need no barrier between them",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const pipeline = co_await shaders::footprint.scale.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::traffic>();
    auto const values = ctx->persistent.create_buffer_from_data(
        ramp(), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const warm_result = make_buffer(*ctx);
    auto const result_a = make_buffer(*ctx);
    auto const result_b = make_buffer(*ctx);

    // Settles `values` after whatever filled it: the first list to read it pays that entry barrier, and this one is it.
    {
        auto cmd = ctx->create_command_list();
        auto const group = ctx->transient.create_binding_group(
            *cmd, layout,
            shaders::traffic{.values = values.as_readwrite_buffer(), .result = warm_result.as_readwrite_buffer()});
        cmd->compute.bind_pipeline(*pipeline);
        cmd->compute.bind_group(0, *group);
        cmd->compute.dispatch_threads(k_count);
        ctx->submit_command_list(cc::move(cmd));
    }

    auto cmd = ctx->create_command_list();
    auto const group_a = ctx->transient.create_binding_group(
        *cmd, layout, shaders::traffic{.values = values.as_readwrite_buffer(), .result = result_a.as_readwrite_buffer()});
    auto const group_b = ctx->transient.create_binding_group(
        *cmd, layout, shaders::traffic{.values = values.as_readwrite_buffer(), .result = result_b.as_readwrite_buffer()});
    auto const before = ctx->metrics.stats();
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group_a);
    cmd->compute.dispatch_threads(k_count);
    // `values` is bound read-write both times and only ever loaded; each result is written once.
    cmd->compute.bind_group(0, *group_b);
    cmd->compute.dispatch_threads(k_count);
    ctx->submit_command_list(cc::move(cmd));
    auto const d = ctx->metrics.stats() - before;

    // What the dispatches computed, read back in a list of its own so its barrier is not in the reading.
    auto readback = ctx->create_command_list();
    auto const future = readback->download.data_from_buffer(result_b);
    ctx->submit_command_list(cc::move(readback));
    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(k_count));
    CHECK(data[5] == 10.0f);

    sg_test::require_counted(d, sg::stat::buffer_barriers);
    CHECK(d[sg::stat::buffer_barriers] == 0);
}

ASYNC_INVOCABLE_TEST("sg - a dispatch that reads what the one before it wrote pays exactly one barrier",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const fill = co_await shaders::footprint.fill.acquire_pipeline(*ctx);
    auto const scale = co_await shaders::footprint.scale.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::traffic>();
    auto const values = make_buffer(*ctx);
    auto const result = make_buffer(*ctx);

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, shaders::traffic{.values = values.as_readwrite_buffer(), .result = result.as_readwrite_buffer()});
    auto const before = ctx->metrics.stats();
    // `fill` writes values and leaves result alone; `scale` reads values and writes result for the first time.
    cmd->compute.bind_pipeline(*fill);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(k_count);
    cmd->compute.bind_pipeline(*scale);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(k_count);
    ctx->submit_command_list(cc::move(cmd));
    auto const d = ctx->metrics.stats() - before;

    auto readback = ctx->create_command_list();
    auto const future = readback->download.data_from_buffer(result);
    ctx->submit_command_list(cc::move(readback));
    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(k_count));
    CHECK(data[7] == 14.0f);

    sg_test::require_counted(d, sg::stat::buffer_barriers);
    CHECK(d[sg::stat::buffer_barriers] == 1);
}

ASYNC_INVOCABLE_TEST("sg - a dispatch that reads what an inline upload wrote pays exactly one barrier",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const scale = co_await shaders::footprint.scale.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::traffic>();
    auto const values = make_buffer(*ctx);
    auto const result = make_buffer(*ctx);
    auto const data_in = ramp();

    auto cmd = ctx->create_command_list();
    auto const group = ctx->transient.create_binding_group(
        *cmd, layout, shaders::traffic{.values = values.as_readwrite_buffer(), .result = result.as_readwrite_buffer()});
    auto const before = ctx->metrics.stats();
    cmd->upload.data_to_buffer(values, cc::span<float const>(data_in));
    cmd->compute.bind_pipeline(*scale);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(k_count);
    ctx->submit_command_list(cc::move(cmd));
    auto const d = ctx->metrics.stats() - before;

    auto readback = ctx->create_command_list();
    auto const future = readback->download.data_from_buffer(result);
    ctx->submit_command_list(cc::move(readback));
    auto const data = co_await future.data();
    REQUIRE(data.size() == isize(k_count));
    CHECK(data[3] == 6.0f);

    sg_test::require_counted(d, sg::stat::buffer_barriers);
    CHECK(d[sg::stat::buffer_barriers] == 1);
}

ASYNC_INVOCABLE_TEST("sg - a draw pays for the buffer its vertex stage reads, and nothing for a texture no stage "
                     "samples",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const fill = co_await shaders::footprint.fill.acquire_pipeline(*ctx);
    auto const traffic_layout = ctx->cached.acquire_binding_group_layout<shaders::traffic>();
    auto const shift_layout = ctx->cached.acquire_binding_group_layout<shaders::shift>();
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::footprint.shifted);

    auto const offsets = make_buffer(*ctx);
    auto const unused_result = make_buffer(*ctx);
    auto const corners
        = ctx->persistent.create_buffer_from_data(cc::vector<shaders::shift_corner>{{.position = tg::vec3f(-1, -1, 0)},
                                                                                    {.position = tg::vec3f(3, -1, 0)},
                                                                                    {.position = tg::vec3f(-1, 3, 0)}},
                                                  sg::buffer_usage::vertex_buffer);
    auto const unsampled
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = 4,
                                             .height = 4,
                                             .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto const image
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                             .width = 4,
                                             .height = 4,
                                             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    auto const texels = cc::vector<byte>::create_defaulted(4 * 4 * 4);

    // Settles every texture: `unsampled` is left in the layout its upload needed, and the target in its own.
    {
        auto cmd = ctx->create_command_list();
        cmd->upload.bytes_to_texture(unsampled.raw(), texels);
        {
            auto pass = cmd->raster.render_to(
                shaders::shift_target{.color = image.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 1))});
        }
        ctx->submit_command_list(cc::move(cmd));
    }

    auto cmd = ctx->create_command_list();
    auto const traffic = ctx->transient.create_binding_group(
        *cmd, traffic_layout,
        shaders::traffic{.values = offsets.as_readwrite_buffer(), .result = unused_result.as_readwrite_buffer()});
    auto const shift = ctx->transient.create_binding_group(
        *cmd, shift_layout,
        shaders::shift{.offsets = offsets.as_readonly_buffer(), .unsampled = unsampled.as_texture_view()});
    auto const before = ctx->metrics.stats();
    cmd->compute.bind_pipeline(*fill);
    cmd->compute.bind_group(0, *traffic);
    cmd->compute.dispatch_threads(k_count);
    {
        auto pass = cmd->raster.render_to(shaders::shift_target{.color = image.as_render_target_view()});
        pass.bind_pipeline(*pipeline);
        pass.bind_group(0, *shift);
        pass.bind_vertex_buffers({corners.as_vertex_buffer()});
        pass.draw({.vertex_range = {.offset = 0, .size = 3}, .instance_range = {.offset = 0, .size = 1}});
    }
    ctx->submit_command_list(cc::move(cmd));
    auto const d = ctx->metrics.stats() - before;

    sg_test::require_counted(d, sg::stat::buffer_barriers);
    // The compute write to `offsets`, ordered before the vertex stage that reads it.
    CHECK(d[sg::stat::buffer_barriers] == 1);
    // Bound, and never sampled: it stays in its copy layout rather than being moved for a read that never happens.
    CHECK(d[sg::stat::texture_barriers] == 0);

    // Nothing above waits on the draw, so the test settles it before it ends.
    co_await ctx->idle_completion();
}
