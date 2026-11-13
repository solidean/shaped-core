#include "../shaders/shader_fixtures.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// The SGL fixture in tests/shaders/sgl/splat_fitting.sgl: differentiable rendering at its smallest.
// Sixteen 2D Gaussians start on a grid and are fitted to two soft shapes, every gradient from one `bwd_diff` per pixel.

namespace
{
constexpr auto k_side = 32;
constexpr auto k_splats = 16;
constexpr auto k_floats = 8;
constexpr auto k_rounds = 200;

/// A red disk and a blue ellipse with soft edges, three floats a pixel.
cc::vector<f32> target_image()
{
    auto out = cc::vector<f32>();
    for (auto y = 0; y < k_side; ++y)
        for (auto x = 0; x < k_side; ++x)
        {
            auto const px = (f32(x) + 0.5f) / f32(k_side);
            auto const py = (f32(y) + 0.5f) / f32(k_side);
            auto const r2 = (px - 0.3f) * (px - 0.3f) + (py - 0.35f) * (py - 0.35f);
            auto const disk = cc::clamp((0.04f - r2) * 50.0f, 0.0f, 1.0f);
            auto const ex = (px - 0.7f) / 0.2f;
            auto const ey = (py - 0.65f) / 0.1f;
            auto const ellipse = cc::clamp((1.0f - (ex * ex + ey * ey)) * 2.0f, 0.0f, 1.0f);
            out.push_back(disk);
            out.push_back(0.0f);
            out.push_back(ellipse);
        }
    return out;
}

/// Sixteen small gray splats on a 4 by 4 grid.
cc::vector<f32> initial_splats()
{
    auto out = cc::vector<f32>();
    for (auto k = 0; k < k_splats; ++k)
    {
        f32 const splat[] = {(f32(k % 4) + 0.5f) / 4.0f,
                             (f32(k / 4) + 0.5f) / 4.0f,
                             -2.3f, // about ln 0.1
                             -2.3f,
                             0.3f * f32(k),
                             0.3f,
                             0.3f,
                             0.3f};
        for (auto const v : splat)
            out.push_back(v);
    }
    return out;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - SGL's bwd_diff fits sixteen Gaussians to an image, the loss falling below a tenth",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    namespace fit = shaders::splat_fitting;
    auto const backward = co_await fit::fit_backward.acquire_pipeline(*ctx);
    auto const step = co_await fit::fit_step.acquire_pipeline(*ctx);

    auto constexpr rw
        = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_src;
    auto const params = ctx->persistent.create_buffer_from_data(initial_splats(), rw);
    auto const target = ctx->persistent.create_buffer_from_data(target_image(), sg::buffer_usage::readonly_buffer);
    auto const zeros = cc::vector<f32>::create_filled(k_splats * k_floats, 0.0f);
    auto const gradients = ctx->persistent.create_buffer_from_data(zeros, rw);
    auto const first = ctx->persistent.create_buffer_from_data(zeros, rw);
    auto const second = ctx->persistent.create_buffer_from_data(zeros, rw);
    auto const losses = ctx->persistent.create_buffer_from_data(cc::vector<f32>::create_filled(k_rounds, 0.0f), rw);

    auto cmd = ctx->create_command_list();
    auto const scene = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<fit::scene>(),
        fit::scene{.params = params.as_readonly_buffer(), .target = target.as_readonly_buffer()});
    auto const fitting = ctx->transient.create_binding_group(
        *cmd, ctx->cached.acquire_binding_group_layout<fit::fitting>(),
        fit::fitting{.gradients = gradients.as_readwrite_buffer(), .losses = losses.as_readwrite_buffer()});
    auto const update = ctx->transient.create_binding_group(*cmd, ctx->cached.acquire_binding_group_layout<fit::update>(),
                                                            fit::update{.params = params.as_readwrite_buffer(),
                                                                        .gradients = gradients.as_readwrite_buffer(),
                                                                        .first = first.as_readwrite_buffer(),
                                                                        .second = second.as_readwrite_buffer()});
    for (auto round = 0; round < k_rounds; ++round)
    {
        cmd->compute.bind_pipeline(*backward);
        cmd->compute.bind_group(0, *scene);
        cmd->compute.bind_group(1, *fitting);
        cmd->compute.set_inline_constants(fit::round{.iteration = round});
        cmd->compute.dispatch_threads(k_side * k_side);
        cmd->compute.bind_pipeline(*step);
        cmd->compute.bind_group(0, *update);
        cmd->compute.dispatch_threads(k_splats * k_floats);
    }
    auto const future = cmd->download.data_from_buffer(losses);
    ctx->submit_command_list(cc::move(cmd));

    auto const l = co_await future.data();
    REQUIRE(l.size() == k_rounds);
    CC_LOG_INFO("splat fitting: loss {} at the start, {} after {} rounds", l[0], l[k_rounds - 1], k_rounds);
    CHECK(l[0] > 0.0f);
    CHECK(l[k_rounds - 1] < 0.1f * l[0]);
    // falling, not by luck at the last round: the last tenth is below the first tenth throughout
    for (auto i = 0; i < k_rounds / 10; ++i)
        CHECK(l[k_rounds - 1 - i] < l[i]);
}
