#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/present/swapchain.hh>
#include <typed-geometry/linalg/vec.hh>

/// Vulkan implementation of sg::swapchain over a VkSwapchainKHR.
///
/// Each back buffer is wrapped in a vulkan_texture with *borrowed* storage, so it flows through the normal rendering
/// and barrier path — which is what leaves its canonical layout at `present` for the next frame, rather than needing
/// a special case at every use.
///
/// **The acquire handshake is where this diverges most from dx12.**
/// DXGI lets the app ask which back buffer is current (`GetCurrentBackBufferIndex`) and gates reuse with a fence;
/// `vkAcquireNextImageKHR` *returns* the index instead, and signals a semaphore the first submit must wait on.
/// So the index is an output here and an input there, and the wait is a queue-side semaphore rather than a host-side
/// fence.
///
/// A **headless** chain is a real VkSwapchainKHR over a VK_EXT_headless_surface — not an emulation.
/// Presenting completes the frame and rotates, and the presented image stays readable, which is what makes headless
/// present pixel-verifiable rather than merely non-crashing.
class sg::backend::vulkan::vulkan_swapchain final : public sg::swapchain
{
public:
    vulkan_swapchain(vulkan_context& ctx, sg::swapchain_description const& desc, VkSurfaceKHR surface)
      : sg::swapchain(desc), _ctx(ctx), _surface(surface)
    {
    }

    /// Creates the surface and the chain, and wraps its images.
    /// Reports rather than asserts: a device without the swapchain extension, or a platform whose window handle sg
    /// cannot yet express, are both runtime answers a caller can act on.
    [[nodiscard]] static cc::result<vulkan_swapchain_handle> create(vulkan_context& ctx,
                                                                    sg::swapchain_description const& desc);

    // Waits for the device to go idle, then destroys the chain and its surface.
    ~vulkan_swapchain() override;

    [[nodiscard]] sg::render_target_view acquire_backbuffer() override;

protected:
    void record_present_transition(sg::command_list& cmd) override;
    void present() override;

private:
    /// Builds (or rebuilds) the chain at `size` and wraps its images.
    /// Passing the old chain in lets the driver reuse its resources, which is also what makes a resize legal while
    /// the old images are still referenced.
    [[nodiscard]] cc::result<cc::unit> build(tg::vec2i size);

    /// The window's current size, or the fixed headless extent.
    [[nodiscard]] tg::vec2i current_extent() const;

    vulkan_context& _ctx; // creating context — must outlive this swapchain (sg lifetime contract)
    VkSurfaceKHR _surface = VK_NULL_HANDLE;
    VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
    VkFormat _vk_format = VK_FORMAT_UNDEFINED;

    cc::vector<vulkan_texture_handle> _backbuffers;

    /// One acquire semaphore per frame in flight, and one render-finished semaphore per back buffer.
    ///
    /// The split is not decoration: an acquire semaphore is signaled before its image index is known, so it cannot be
    /// indexed by image; a render-finished semaphore is waited on by the present of one specific image, so it must
    /// be.
    cc::vector<VkSemaphore> _acquire_semaphores;
    cc::vector<VkSemaphore> _render_semaphores;
    int _frame = 0;

    tg::vec2i _size = tg::vec2i(0, 0);
    sg::epoch _last_resize_epoch = sg::epoch::invalid;
    u32 _acquired_index = 0;
    bool _acquired = false; // true between acquire_backbuffer and present (enforces the 1:1 pairing)
};
