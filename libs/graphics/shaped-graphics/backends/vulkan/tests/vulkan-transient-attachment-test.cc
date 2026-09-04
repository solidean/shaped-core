#include "vulkan-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

// A transient attachment recreated every epoch, with epochs actually pipelined — a per-frame depth buffer or
// G-buffer, which is what every frame loop builds.
//
// The hazard is in the VkImageView cache rather than in the GPU work.
// A cached view is dropped by its texture's own finalizer, which runs when the VkImage is destroyed at epoch retire;
// the texture *object* dies a full pipeline depth earlier, and the allocator hands its address straight to the next
// transient texture.
// An entry keyed on that address is therefore inherited by a different texture, so the frame renders through a view
// of an image that is about to be freed under the GPU — a use-after-free that surfaces as VK_ERROR_DEVICE_LOST
// several frames later, nowhere near its cause.
//
// Nothing is read back inside the loop on purpose: a blocking readback retires every epoch immediately, which closes
// the very window the test exists to hold open.

using namespace cc::primitive_defines;

namespace
{
namespace vulkan = sg::backend::vulkan;

constexpr int k_extent = 64;
constexpr int k_frames = 120;

/// Deep enough that a frame's texture is reclaimed while a later frame is still in flight, which is the window.
constexpr int k_epochs_in_flight = 2;
} // namespace

TEST("sg vulkan - a transient attachment recreated every pipelined epoch")
{
    auto handle = vulkan::test::make_context();
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& ctx = *handle;

    // Persistent, so the loop below has something whose contents outlive a frame — the transient attachment is the
    // depth buffer, exactly as in a frame loop.
    auto color = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                                   .width = k_extent,
                                                   .height = k_extent,
                                                   .usage = sg::texture_usage::render_target});
    REQUIRE(color.raw() != nullptr);

    for (int frame = 0; frame < k_frames; ++frame)
    {
        auto depth = ctx.transient.create_texture_2d({.format = sg::pixel_format::depth32_float,
                                                      .width = k_extent,
                                                      .height = k_extent,
                                                      .usage = sg::texture_usage::depth_stencil});
        REQUIRE(depth.raw() != nullptr);

        auto cmd = ctx.create_command_list();
        REQUIRE(cmd != nullptr);
        {
            auto pass = cmd->raster.render_to(
                {.color_targets = {color.as_render_target_view().cleared(tg::vec4f(0.2f, 0.4f, 0.6f, 1.0f))},
                 .depth_stencil_target = depth.as_depth_stencil_view().cleared(1.0f)});
        }
        ctx.submit_command_list(cc::move(cmd));

        ctx.advance_epoch(k_epochs_in_flight);
        REQUIRE(!ctx.is_device_lost()).context(cc::format("frame {}", frame));
    }

    ctx.advance_epoch_and_wait_for_idle();
    CHECK(!ctx.is_device_lost());
}
