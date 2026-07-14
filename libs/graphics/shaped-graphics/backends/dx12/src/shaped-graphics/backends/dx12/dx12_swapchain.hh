#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/swapchain.hh>

namespace sg::backend::dx12
{
/// DirectX 12 implementation of sg::swapchain over an IDXGISwapChain3 (flip-discard model). Each back
/// buffer is wrapped in a dx12_texture so it flows through the normal render-pass / barrier path (which
/// creates the RTV on demand); a dedicated present fence gates back-buffer reuse. Auto-resizes to its
/// HWND's client area on acquire. Created by dx12_context::create_dx12_swapchain.
class dx12_swapchain final : public sg::swapchain
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

    // Drops the back-buffer wrappers (deferred deletion gates their real release on GPU completion) and
    // closes the fence event. Body in dx12_swapchain.cc.
    ~dx12_swapchain() override;

    // sg::swapchain overrides
    [[nodiscard]] sg::render_target_view acquire_backbuffer() override;
    void present() override;
    [[nodiscard]] tg::vec2i get_size() const override { return _size; }
    [[nodiscard]] int get_width() const override { return _size[0]; }
    [[nodiscard]] int get_height() const override { return _size[1]; }

    // Populates _backbuffers (one dx12_texture wrapper per buffer) from the current IDXGISwapChain3 at the
    // current _size. Called at creation and after every ResizeBuffers. Returns an error if GetBuffer fails.
    [[nodiscard]] cc::result<cc::unit> build_backbuffers();

private:
    // One back buffer: the dx12_texture wrapping the DXGI resource (its RTV is created on demand by the
    // render pass), plus the present-fence value that must complete before this buffer is reused.
    struct backbuffer
    {
        dx12_texture_handle texture;
        cc::u64 frame_fence_value = 0;
    };

    // Drops the back-buffer wrappers (their ~dx12_texture schedules the DXGI resource for deferred
    // deletion). Does not drain the GPU — callers that must (resize) do so around it.
    void release_backbuffers();

    // Drains the GPU, releases the back buffers, calls ResizeBuffers to `size`, and rebuilds. The idle
    // drain + deferred-deletion sweep before ResizeBuffers is what leaves zero outstanding back-buffer
    // references (D3D12 requires it).
    [[nodiscard]] cc::result<cc::unit> resize(tg::vec2i size);

    // Marks the context device-lost if `hr` indicates removal, then throws: device_lost_exception when the
    // device is lost, else a generic sg::exception carrying `hr`. For the throwing frame path.
    [[noreturn]] void fail(HRESULT hr, char const* what);

    dx12_context& _ctx; // creating context — must outlive this swapchain (sg lifetime contract)
    HWND _hwnd;
    ComPtr<IDXGISwapChain3> _swapchain;
    bool _tearing = false; // ALLOW_TEARING negotiated (immediate present-mode + adapter support)

    cc::vector<backbuffer> _backbuffers;

    // Present timeline on the context's direct queue: signaled after each Present so the next acquire of a
    // given back-buffer index waits for its prior frame to finish.
    ComPtr<ID3D12Fence> _present_fence;
    cc::u64 _fence_value = 0;
    HANDLE _fence_event = nullptr;

    tg::vec2i _size;        // current back-buffer resolution (tracks auto-resize)
    bool _acquired = false; // true between acquire_backbuffer and present (enforces the 1:1 pairing)
};
} // namespace sg::backend::dx12
