#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/present/swapchain.hh>

/// Metal implementation of sg::swapchain, over a CAMetalLayer — or over plain textures when there is no window.
///
/// **Headless is an emulation here, and that is not a shortcut.**
/// `VK_EXT_headless_surface` gives vulkan a genuine swapchain with no display, so its headless path exercises every
/// step the windowed one does.
/// Metal has no counterpart: a drawable comes from a layer and a layer needs a window.
/// So a headless chain is a ring of ordinary render-target textures with a rotating index, exactly as dx12 emulates it
/// — which means the two paths share the acquire and diverge at present.
///
/// **The present handshake needs both halves of sg's split.**
/// `MTL4CommandQueue::waitForDrawable` must run before the work that draws into the drawable, and `signalDrawable`
/// after it — so the submit that carries the frame is what sits between them, which is precisely the room
/// `record_present_transition` + `present` leaves.
class sg::backend::metal::metal_swapchain final : public sg::swapchain
{
public:
    metal_swapchain(metal_context& ctx, sg::swapchain_description const& desc, CA::MetalLayer* layer);
    ~metal_swapchain() override;

    [[nodiscard]] render_target_view acquire_backbuffer() override;

private:
    void record_present_transition(command_list& cmd) override;
    void present() override;

    /// Rebuild the back buffers at `size`, dropping whatever was there.
    /// Windowed chains resize their layer; a headless one never resizes at all.
    void recreate(tg::vec2i size);

    metal_context& _ctx;

    /// Null for a headless chain, which is what makes the two paths distinguishable without a second flag.
    CA::MetalLayer* _layer = nullptr;

    /// The drawable of the frame being rendered, held from acquire until present.
    /// Always null on a headless chain.
    CA::MetalDrawable* _drawable = nullptr;

    /// A headless chain's back buffers, and the one `acquire_backbuffer` last handed out.
    cc::vector<sg::raw_texture_handle> _buffers;
    int _next_buffer = 0;

    tg::vec2i _size = tg::vec2i(0, 0);
    sg::epoch _last_resize_check = sg::epoch::invalid;
};
