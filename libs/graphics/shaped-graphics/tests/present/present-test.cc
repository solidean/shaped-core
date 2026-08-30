#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>

using namespace cc::primitive_defines;

// Backend-agnostic headless presentation: the whole acquire / render / present handshake, with no window.
//
// Tier 1 rather than tier 2 because nothing here reaches into a backend, and because the two backends arrive at
// headless present by different routes — vulkan a real VkSwapchainKHR over VK_EXT_headless_surface, dx12 an
// emulation over ordinary render-target textures.
// What sg promises is the same either way, so it is stated once.
//
// Runs against every available backend (see tests/context/context-test.cc for the mechanism).

namespace
{
constexpr int k_extent = 32;
constexpr int k_buffers = 3;

// Enough frames to wrap the chain twice over, so a buffer is reused rather than merely handed out once.
constexpr int k_frames = 7;
} // namespace

INVOCABLE_TEST("sg - a headless swapchain presents and cycles", (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!ctx->supports_headless_present())
        SKIP("this backend cannot present headlessly");

    // Designators must follow declaration order: headless_extent is declared before buffer_count.
    auto swapchain = ctx->create_swapchain({.headless_extent = tg::vec2i(k_extent, k_extent),
                                            .buffer_count = k_buffers,
                                            .format = sg::pixel_format::bgra8_unorm});
    REQUIRE(swapchain != nullptr);
    CHECK(!swapchain->is_windowed());

    u8 const shades[k_frames] = {10, 40, 70, 100, 130, 160, 190};

    // The back buffer each frame was handed, by address.
    // Reading the rendered pixels back cannot show cycling — it reads the same view it just wrote — so the identity
    // of the acquired texture is what pins that the chain rotates rather than returning one image forever.
    cc::vector<sg::raw_texture const*> acquired;

    bool all_correct = true;
    for (int frame = 0; frame < k_frames; ++frame)
    {
        auto const rt = swapchain->acquire_backbuffer();
        CHECK(rt.width() == k_extent);
        CHECK(rt.height() == k_extent);
        acquired.push_back(rt.texture().get());

        auto cmd = ctx->create_command_list();
        {
            // bgra8: the blue channel is byte 0, so a shade in `b` is what the readback's first byte carries.
            float const b = float(shades[frame]) / 255.0f;
            auto pass = cmd->raster.render_to({.color_targets = {rt.cleared(tg::vec4f(0, 0, b, 1))}});
        }

        // Read the frame back BEFORE presenting it: after the present the image belongs to the presentation engine,
        // and a headless chain's promise is that the frame completes, not that its buffer stays borrowable.
        auto future = cmd->download.bytes_from_texture(rt.texture());
        ctx->submit_command_list_and_present(*swapchain, cc::move(cmd));

        auto const pixels = ctx->wait_for(future);
        REQUIRE(pixels.has_value());
        auto const* const p = reinterpret_cast<u8 const*>(pixels.value().data());
        if (p[0] != shades[frame] || p[3] != 255)
            all_correct = false;

        ctx->advance_epoch(2);
    }
    CHECK(all_correct);

    // Every back buffer was handed out at least once over seven frames on a three-deep chain.
    auto distinct = cc::vector<sg::raw_texture const*>();
    for (auto const* const t : acquired)
    {
        bool seen = false;
        for (auto const* const d : distinct)
            seen = seen || d == t;
        if (!seen)
            distinct.push_back(t);
    }
    CHECK(distinct.size() == k_buffers).context("the chain must rotate through every back buffer");

    ctx->advance_epoch_and_wait_for_idle();
}
