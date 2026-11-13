#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// The SGL fixture in tests/shaders/sgl/differentiable_buffers.sgl.
// At x = 0.5 with weights 1, 2, 3, 4, each loss's d_k = w_k / 2 - 1 is -0.5, 0, 0.5, 1, and its gradient of w_k is
// 2 d_k x = d_k, its change along tangent t_k is d_k t_k.

ASYNC_INVOCABLE_TEST("sg - a buffer with a @gradient takes every invocation's adjoint, and one with a @tangent its "
                     "change",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const backward = co_await shaders::differentiable_buffers::loss_backward.acquire_pipeline(*ctx);
    auto const forward = co_await shaders::differentiable_buffers::loss_forward.acquire_pipeline(*ctx);
    auto const params_layout = ctx->cached.acquire_binding_group_layout<shaders::differentiable_buffers::params>();
    auto const training_layout = ctx->cached.acquire_binding_group_layout<shaders::differentiable_buffers::training>();

    constexpr auto count = 4096;
    auto const weights = ctx->persistent.create_buffer_from_data(cc::vector<f32>{1.0f, 2.0f, 3.0f, 4.0f},
                                                                 sg::buffer_usage::readonly_buffer);
    auto const tangents = ctx->persistent.create_buffer_from_data(cc::vector<f32>{1.0f, 2.0f, 3.0f, 4.0f},
                                                                  sg::buffer_usage::readonly_buffer);
    auto const gradients = ctx->persistent.create_buffer_from_data(
        cc::vector<f32>::create_filled(4, 0.0f), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const changes = ctx->persistent.create_buffer_from_data(
        cc::vector<f32>::create_filled(count, 0.0f), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const params = ctx->transient.create_binding_group(
        *cmd, params_layout, shaders::differentiable_buffers::params{.weights = weights.as_readonly_buffer()});
    auto const training = ctx->transient.create_binding_group(
        *cmd, training_layout,
        shaders::differentiable_buffers::training{.gradients = gradients.as_readwrite_buffer(),
                                                  .tangents = tangents.as_readonly_buffer(),
                                                  .forward = changes.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*backward);
    cmd->compute.bind_group(0, *params);
    cmd->compute.bind_group(1, *training);
    cmd->compute.dispatch_threads(count);
    cmd->compute.bind_pipeline(*forward);
    cmd->compute.bind_group(0, *params);
    cmd->compute.bind_group(1, *training);
    cmd->compute.dispatch_threads(count);
    auto const gradient_future = cmd->download.data_from_buffer(gradients);
    auto const change_future = cmd->download.data_from_buffer(changes);
    ctx->submit_command_list(cc::move(cmd));

    // each weight is read by a quarter of the invocations, and every partial sum is exact in a float
    auto const g = co_await gradient_future.data();
    REQUIRE(g.size() == 4);
    CHECK(g[0] == -512.0f);
    CHECK(g[1] == 0.0f);
    CHECK(g[2] == 512.0f);
    CHECK(g[3] == 1024.0f);

    auto const c = co_await change_future.data();
    REQUIRE(c.size() == count);
    for (auto i = 0; i < 8; ++i)
    {
        f32 const expected[] = {-0.5f, 0.0f, 1.5f, 4.0f};
        CHECK(c[i] == expected[i % 4]);
    }
}

// Mark 0 at (2, 0, 0) of weight 1, mark 1 at (1, 2, 0) of weight 0.5, each the loss weight * |at - (1, 0, 0)|^2.
// Each invocation adds 2 weight (at - (1, 0, 0)) and |at - (1, 0, 0)|^2: (2, 0, 0) and 1, then (0, 2, 0) and 4.
ASYNC_INVOCABLE_TEST("sg - a buffer of structs with a @gradient takes an atomic per float of its tangent, in order",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    namespace shader = shaders::differentiable_buffers;
    auto const backward = co_await shader::marks_backward.acquire_pipeline(*ctx);
    constexpr auto count = 4096;
    auto const marks = ctx->persistent.create_buffer_from_data(
        cc::vector<shader::mark>{{.at = tg::pos3f(2.0f, 0.0f, 0.0f), .weight = 1.0f},
                                 {.at = tg::pos3f(1.0f, 2.0f, 0.0f), .weight = 0.5f}},
        sg::buffer_usage::readonly_buffer);
    auto const gradients = ctx->persistent.create_buffer_from_data(
        cc::vector<f32>::create_filled(8, 0.0f), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const marked
        = ctx->transient.create_binding_group(*cmd, ctx->cached.acquire_binding_group_layout<shader::marked>(),
                                              shader::marked{.marks = marks.as_readonly_buffer()});
    auto const training
        = ctx->transient.create_binding_group(*cmd, ctx->cached.acquire_binding_group_layout<shader::mark_training>(),
                                              shader::mark_training{.gradients = gradients.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*backward);
    cmd->compute.bind_group(0, *marked);
    cmd->compute.bind_group(1, *training);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer(gradients);
    ctx->submit_command_list(cc::move(cmd));

    auto const g = co_await future.data();
    REQUIRE(g.size() == 8);
    f32 const expected[] = {4096.0f, 0.0f, 0.0f, 2048.0f, 0.0f, 4096.0f, 0.0f, 8192.0f};
    for (auto i = 0; i < 8; ++i)
        CHECK(g[i] == expected[i]);
}

// As above, where a subgroup sums its adjoints first: the index is one per 64 invocations, so each subgroup adds to
// one mark, and a third of its invocations never reach the add, which a sum over the subgroup must leave out.
ASYNC_INVOCABLE_TEST("sg - a declared subgroups shares a gradient's atomic through the subgroup, over the invocations "
                     "that reach it",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    if (!ctx->supports(sg::feature::subgroups))
        SKIP("the device has no subgroup operations");

    namespace shader = shaders::differentiable_buffers;
    auto const backward = co_await shader::marks_backward_reduced.acquire_pipeline(*ctx);
    constexpr auto count = 4096;
    auto const marks = ctx->persistent.create_buffer_from_data(
        cc::vector<shader::mark>{{.at = tg::pos3f(2.0f, 0.0f, 0.0f), .weight = 1.0f},
                                 {.at = tg::pos3f(1.0f, 2.0f, 0.0f), .weight = 0.5f}},
        sg::buffer_usage::readonly_buffer);
    auto const gradients = ctx->persistent.create_buffer_from_data(
        cc::vector<f32>::create_filled(8, 0.0f), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    auto const marked
        = ctx->transient.create_binding_group(*cmd, ctx->cached.acquire_binding_group_layout<shader::marked>(),
                                              shader::marked{.marks = marks.as_readonly_buffer()});
    auto const training
        = ctx->transient.create_binding_group(*cmd, ctx->cached.acquire_binding_group_layout<shader::mark_training>(),
                                              shader::mark_training{.gradients = gradients.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*backward);
    cmd->compute.bind_group(0, *marked);
    cmd->compute.bind_group(1, *training);
    cmd->compute.dispatch_threads(count);
    auto const future = cmd->download.data_from_buffer(gradients);
    ctx->submit_command_list(cc::move(cmd));

    // each reaching invocation of mark 0 adds (2, 0, 0) and 1, of mark 1 (0, 2, 0) and 4
    f32 reached[2] = {0.0f, 0.0f};
    for (auto i = 0; i < count; ++i)
        if (i % 3 != 0)
            reached[(i / 64) % 2] += 1.0f;
    auto const g = co_await future.data();
    REQUIRE(g.size() == 8);
    f32 const expected[]
        = {2.0f * reached[0], 0.0f, 0.0f, reached[0], 0.0f, 2.0f * reached[1], 0.0f, 4.0f * reached[1]};
    for (auto i = 0; i < 8; ++i)
        CHECK(g[i] == expected[i]);
}

// One row of four texels, 0, 10, 20 and 30, sampled at u = 0.5 by every invocation: halfway between texels 1 and 2,
// so each takes half of the 2 the loss has, and the coordinate's slope is 2 * 4 * (20 - 10).
ASYNC_INVOCABLE_TEST("sg - a texture with a @gradient takes a linear sample's adjoint at the texels it weighs",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    namespace shader = shaders::differentiable_buffers;
    auto const backward = co_await shader::canvas_backward.acquire_pipeline(*ctx);
    constexpr auto count = 4096;
    auto const canvas = sg::texture_2d::from_raw(ctx->persistent.create_raw_texture({
        .format = sg::pixel_format::r32_float,
        .dimension = sg::texture_dimension::d2,
        .width = 4,
        .height = 1,
        .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst,
    }));
    auto const texels = cc::vector<f32>{0.0f, 10.0f, 20.0f, 30.0f};
    auto const gradients = ctx->persistent.create_buffer_from_data(
        cc::vector<f32>::create_filled(4, 0.0f), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const slopes = ctx->persistent.create_buffer_from_data(
        cc::vector<f32>::create_filled(count, 0.0f), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(canvas.raw(), cc::as_bytes(texels));
    auto const painted
        = ctx->transient.create_binding_group(*cmd, ctx->cached.acquire_binding_group_layout<shader::painted>(),
                                              shader::painted{.canvas = canvas.as_texture_view()});
    auto const painting = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<shader::painting>(),
        shader::painting{.gradients = gradients.as_readwrite_buffer(), .slopes = slopes.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*backward);
    cmd->compute.bind_group(0, *painted);
    cmd->compute.bind_group(1, *painting);
    cmd->compute.dispatch_threads(count);
    auto const gradient_future = cmd->download.data_from_buffer(gradients);
    auto const slope_future = cmd->download.data_from_buffer(slopes);
    ctx->submit_command_list(cc::move(cmd));

    auto const g = co_await gradient_future.data();
    REQUIRE(g.size() == 4);
    CHECK(g[0] == 0.0f);
    CHECK(g[1] == 4096.0f);
    CHECK(g[2] == 4096.0f);
    CHECK(g[3] == 0.0f);
    auto const s = co_await slope_future.data();
    REQUIRE(s.size() == count);
    CHECK(s[0] == 80.0f);
    CHECK(s[count - 1] == 80.0f);
}
