#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/present/swapchain.hh>

/// DirectX 12 implementation of sg::swapchain over an IDXGISwapChain3, flip-discard model.
///
/// **A headless chain has no IDXGISwapChain3 at all.**
/// DXGI needs a real presentation target, so there is nothing to create one over — the chain becomes `buffer_count`
/// ordinary render-target textures and a present that signals the fence and rotates the index.
/// That is an emulation, where the vulkan backend gets a real VkSwapchainKHR over VK_EXT_headless_surface; what both
/// guarantee is the same, which is what sg's contract asks for: the frame completes, the chain cycles, and the
/// presented buffer stays readable until its epoch retires.
/// Each back buffer is wrapped in a dx12_texture with borrowed storage, so it flows through the normal render-pass / barrier path.
/// The RTV is created on demand by the render pass.
/// A dedicated present fence gates back-buffer reuse.
/// Auto-resizes to its HWND's client area, checked at most once per epoch so acquire never advances an epoch under the caller.
/// Created by dx12_context::create_dx12_swapchain.
class sg::backend::dx12::dx12_swapchain final : public sg::swapchain
{
public:
    dx12_swapchain(dx12_context& ctx,
                   sg::swapchain_description const& desc,
                   HWND hwnd,
                   ComPtr<IDXGISwapChain3> swapchain,
                   bool allow_tearing,
                   ComPtr<ID3D12Fence> present_fence,
                   HANDLE fence_event,
                   tg::vec2i size)
      : sg::swapchain(desc),
        _ctx(ctx),
        _hwnd(hwnd),
        _swapchain(cc::move(swapchain)),
        _tearing(allow_tearing),
        _present_fence(cc::move(present_fence)),
        _fence_event(fence_event),
        _size(size)
    {
    }

    // Waits for the GPU to finish with the back buffers, then releases them — borrowed storage, so synchronously — and closes the fence event.
    ~dx12_swapchain() override;

    [[nodiscard]] sg::render_target_view acquire_backbuffer() override;

    // Populates _backbuffers at the current _size: one wrapper per DXGI buffer, or one owned render-target texture
    // per buffer on a headless chain.
    // Called at creation and after every ResizeBuffers.
    // Returns an error when GetBuffer or the texture allocation fails.
    [[nodiscard]] cc::result<cc::unit> build_backbuffers();

protected:
    // sg::swapchain present handshake (driven by context::submit_command_list_and_present).
    void record_present_transition(sg::command_list& cmd) override;
    void present() override;

private:
    // One back buffer: the dx12_texture wrapping the DXGI resource, plus the present-fence value that must complete before this buffer is reused.
    struct backbuffer
    {
        dx12_texture_handle texture;
        u64 frame_fence_value = 0;
    };

    // Blocks until the GPU has finished every present submitted so far, i.e. until the present fence reaches _fence_value.
    // The back buffers are then safe to release synchronously; a no-op when the device is lost.
    void wait_for_gpu();

    // Releases the back-buffer wrappers.
    // Borrowed storage, so ~dx12_texture drops the DXGI reference synchronously — callers must wait_for_gpu() first.
    void release_backbuffers();

    // Waits for the GPU, releases the back buffers, calls ResizeBuffers to `size`, and rebuilds.
    // Does NOT advance an epoch: acquire calls this at most once per epoch, so the present-fence wait alone leaves the zero outstanding back-buffer references ResizeBuffers requires.
    [[nodiscard]] cc::result<cc::unit> resize(tg::vec2i size);

    // Marks the context device-lost when `hr` indicates removal, then throws.
    // device_lost_exception when the device is lost, else a generic sg::exception carrying `hr`.
    [[noreturn]] void fail(HRESULT hr, char const* what);

    dx12_context& _ctx; // creating context — must outlive this swapchain (sg lifetime contract)
    HWND _hwnd;
    ComPtr<IDXGISwapChain3> _swapchain;
    bool _tearing = false; // ALLOW_TEARING negotiated (immediate present-mode + adapter support)

    cc::vector<backbuffer> _backbuffers;

    // Present timeline on the context's direct queue: signaled after each Present, so the next acquire of a given back-buffer index waits for its prior frame to finish.
    ComPtr<ID3D12Fence> _present_fence;
    u64 _fence_value = 0;
    HANDLE _fence_event = nullptr;

    tg::vec2i _size;                                   // current back-buffer resolution (tracks auto-resize)
    sg::epoch _last_resize_epoch = sg::epoch::invalid; // epoch of the last auto-resize check (once per epoch)
    UINT _acquired_index = 0;                          // back-buffer index handed out by the current acquire
    bool _acquired = false; // true between acquire_backbuffer and present (enforces the 1:1 pairing)
};
