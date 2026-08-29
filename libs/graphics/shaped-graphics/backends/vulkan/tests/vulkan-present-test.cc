#include "vulkan-test-common.hh"

#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// Headless presentation: the whole acquire / render / transition / present handshake, with no window.
//
// This is a real VkSwapchainKHR over a VK_EXT_headless_surface rather than an emulation, so every step the windowed
// path takes is taken here — vkAcquireNextImageKHR returning an index, the submit waiting on the acquire semaphore
// and signalling the render-finished one, and vkQueuePresentKHR waiting on that.
// What headless removes is only the display.
//
// The chain also *cycles*: each frame clears to a different color and reads the result back, so a swapchain that
// handed out the same image every frame, or presented one and rendered into another, would fail.

namespace
{
namespace vulkan = sg::backend::vulkan;

constexpr int k_extent = 32;
} // namespace

TEST("sg vulkan - a headless swapchain presents and cycles")
{
    auto handle = vulkan::test::make_context();
    if (handle == nullptr)
        SKIP("no vulkan device");
    auto& ctx = *handle;
    if (!static_cast<vulkan::vulkan_context&>(ctx).is_headless_present_supported())
        SKIP("no VK_EXT_headless_surface on this instance");

    auto swapchain = ctx.create_swapchain(
        {.buffer_count = 3, .format = sg::pixel_format::bgra8_unorm, .headless_extent = tg::vec2i(k_extent, k_extent)});
    REQUIRE(swapchain != nullptr);
    CHECK(!swapchain->is_windowed());

    // Enough frames to wrap the chain twice over, so a buffer is reused rather than merely handed out once.
    constexpr int k_frames = 7;
    u8 const shades[k_frames] = {10, 40, 70, 100, 130, 160, 190};

    bool all_correct = true;
    for (int frame = 0; frame < k_frames; ++frame)
    {
        auto const rt = swapchain->acquire_backbuffer();
        CHECK(rt.width() == k_extent);
        CHECK(rt.height() == k_extent);

        auto cmd = ctx.create_command_list();
        {
            // bgra8: the blue channel is byte 0, so a shade in `b` is what the readback's first byte carries.
            float const b = float(shades[frame]) / 255.0f;
            auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(tg::vec4f(0, 0, b, 1))}});
        }

        // Read the frame back BEFORE presenting it: after the present the image belongs to the presentation engine,
        // and a headless chain's promise is that the frame completes, not that its buffer stays borrowable.
        auto future = cmd->download.bytes_from_texture(rt.texture());
        ctx.submit_command_list_and_present(*swapchain, cc::move(cmd));

        auto const pixels = ctx.wait_for(future);
        REQUIRE(pixels.has_value());
        auto const* const p = reinterpret_cast<u8 const*>(pixels.value().data());
        if (p[0] != shades[frame] || p[3] != 255)
            all_correct = false;

        ctx.advance_epoch(2);
    }
    CHECK(all_correct);

    ctx.advance_epoch_and_wait_for_idle();
}
