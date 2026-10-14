#include "../pipeline/pipeline_harness.hh"
#include "../pipeline/rects.hh"
#include "../shaders/shader_fixtures.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_view_desc.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/texture.hh>

// D3D12 reads an RTV descriptor when OMSetRenderTargets records it, not when the GPU executes the list.
// Microsoft documents that for CPU descriptor handles, and this pins it on the device the suite runs on.
// It is the premise of reusing a command list's RTV slots straight after binding them, with no epoch-deferred free.
//
// The scope's RTV slot is overwritten with a decoy texture's view after the bind and before the draw.
// A runtime that read the slot late would draw into the decoy; one that read it at the bind leaves the decoy untouched.
// The debug layer is on in every dx12 driver of this binary, so a complaint about the overwrite fails the test too.

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

namespace
{
namespace dx12 = sg::backend::dx12;

constexpr int k_width = 8;
constexpr int k_height = 8;
} // namespace

ASYNC_INVOCABLE_TEST("sg dx12 - an RTV slot overwritten after the bind still draws into the bound target",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (ctx->backend() != sg::backend_kind::dx12)
        SKIP("a dx12 descriptor-lifetime property");
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto& dx = static_cast<dx12::dx12_context&>(*ctx);
    auto const pipeline = co_await ctx->cached.acquire_raster_pipeline(shaders::rects.floating);
    auto const white = tg::vec4f(1, 1, 1, 1);
    sg_test::rect const rects[] = {sg_test::rect_at(0, 0, k_width, k_height, k_width, k_height, 0.5f, white)};
    auto const batch = sg_test::rect_batch(*ctx, rects);

    auto const decoy
        = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba16_float,
                                             .width = k_width,
                                             .height = k_height,
                                             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
    auto decoy_bytes = cc::optional<sg::bytes_future>();

    auto const pixels = co_await sg_test::draw_offscreen_passes(
        *ctx,
        {.width = k_width,
         .height = k_height,
         .colors = {sg::pixel_format::rgba16_float},
         .target_set = shaders::rect_target::name},
        [&](sg::command_list& cmd, sg_test::offscreen_targets const& targets)
        {
            {
                auto clear_decoy = cmd.raster.render_to(
                    {.color_targets = {decoy.as_render_target_view().cleared(tg::vec4f(0, 0, 0, 0))}});
            }

            auto info = targets.cleared();
            info.target_set = shaders::rect_target::name;
            {
                auto scope = cmd.raster.render_to(info);

                auto& list = static_cast<dx12::dx12_command_list&>(cmd);
                REQUIRE(list._rtv_slots.size() == 1);
                dx12::create_render_target_view(dx._device.Get(), decoy.as_render_target_view(),
                                                dx._rtv_heap->cpu_at(list._rtv_slots[0]));

                scope.bind_pipeline(*pipeline);
                batch.draw(scope, 0);
            }

            decoy_bytes = cmd.download.bytes_from_texture(decoy.raw());
        });

    auto const decoy_read = co_await decoy_bytes.value().bytes();
    auto decoy_untouched = true;
    for (auto const b : decoy_read)
        if (b != byte(0))
            decoy_untouched = false;
    CHECK(decoy_untouched);

    for (auto y = 0; y < k_height; ++y)
        for (auto x = 0; x < k_width; ++x)
            CHECK(pixels[0].rgba_float(x, y) == white).context(cc::format("pixel ({}, {})", x, y));
}
