// dx12 swapchain: IDXGISwapChain3 (flip-discard) creation, per-frame acquire/present, and auto-resize.
// Back buffers are wrapped in dx12_texture so they use the normal render-pass + barrier path; a dedicated
// present fence gates back-buffer reuse.

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_format.hh>
#include <shaped-graphics/backends/dx12/dx12_swapchain.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg::backend::dx12
{
namespace
{
// Current client-area size of `hwnd`, clamped to at least 1x1 (a zero-size / minimized window keeps the
// last valid extent — DXGI rejects a zero-size chain).
tg::vec2i client_size_of(HWND hwnd)
{
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int const w = int(rc.right - rc.left);
    int const h = int(rc.bottom - rc.top);
    return tg::vec2i(w > 0 ? w : 1, h > 0 ? h : 1);
}

// Best-effort HDR: pick an HDR color space matching the (wide) back-buffer format and set it if the
// swapchain supports presenting in it. A no-op when unsupported — the surface stays SDR.
void apply_hdr_colorspace(IDXGISwapChain3* swapchain, sg::pixel_format format)
{
    // rgba16_float -> scRGB (linear, Rec.709 primaries); otherwise HDR10 (ST.2084, Rec.2020).
    DXGI_COLOR_SPACE_TYPE const cs = format == sg::pixel_format::rgba16_float
                                       ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                       : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    UINT support = 0;
    if (SUCCEEDED(swapchain->CheckColorSpaceSupport(cs, &support))
        && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        swapchain->SetColorSpace1(cs);
}
} // namespace

cc::result<dx12_swapchain_handle> dx12_context::create_dx12_swapchain(sg::swapchain_description const& desc)
{
    // Validate the contract before any DXGI work, so a bad desc asserts at the entry point.
    desc.assert_valid();

    HWND const hwnd = static_cast<HWND>(desc.native_window_handle);
    tg::vec2i const size = client_size_of(hwnd);

    // Tearing (uncapped present) needs adapter support AND the immediate present mode; it also requires the
    // ALLOW_TEARING swap-chain flag at creation and on every Present / ResizeBuffers.
    BOOL allow_tearing = FALSE;
    if (ComPtr<IDXGIFactory5> factory5; SUCCEEDED(_factory.As(&factory5)))
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing));
    bool const tearing = allow_tearing && desc.present_mode == sg::present_mode::immediate;

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = UINT(size[0]);
    scd.Height = UINT(size[1]);
    scd.Format = to_dxgi_format(desc.format);
    scd.SampleDesc = {.Count = 1, .Quality = 0}; // the flip model does not allow a multisampled back buffer
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = UINT(desc.buffer_count);
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    scd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    ComPtr<IDXGISwapChain1> sc1;
    if (HRESULT hr = _factory->CreateSwapChainForHwnd(_queue.Get(), hwnd, &scd, nullptr, nullptr, &sc1); FAILED(hr))
        return dx12_error(hr, "IDXGIFactory::CreateSwapChainForHwnd failed");

    // The app owns presentation — suppress DXGI's Alt+Enter fullscreen handling.
    _factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<IDXGISwapChain3> sc3;
    if (HRESULT hr = sc1.As(&sc3); FAILED(hr))
        return dx12_error(hr, "IDXGISwapChain3 unavailable (SDK/driver too old)");

    if (desc.enable_hdr)
        apply_hdr_colorspace(sc3.Get(), desc.format);

    ComPtr<ID3D12Fence> present_fence;
    if (HRESULT hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&present_fence)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateFence (swapchain present) failed");

    HANDLE const fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence_event == nullptr)
        return cc::error("CreateEvent for the swapchain present fence failed");

    auto sc = std::make_shared<dx12_swapchain>(*this, desc, hwnd, cc::move(sc3), tearing, cc::move(present_fence),
                                               fence_event, size);
    CC_RETURN_IF_ERROR(sc->build_backbuffers());
    return dx12_swapchain_handle(cc::move(sc));
}

cc::result<cc::unit> dx12_swapchain::build_backbuffers()
{
    _backbuffers.clear();
    _backbuffers.reserve(_desc.buffer_count);
    for (int i = 0; i < _desc.buffer_count; ++i)
    {
        ComPtr<ID3D12Resource> resource;
        if (HRESULT hr = _swapchain->GetBuffer(UINT(i), IID_PPV_ARGS(&resource)); FAILED(hr))
            return dx12_error(hr, "IDXGISwapChain::GetBuffer failed");

        sg::texture_description td;
        td.format = _desc.format;
        td.dimension = sg::texture_dimension::d2;
        td.width = _size[0];
        td.height = _size[1];
        td.usage = sg::texture_usage::render_target;

        // Wrap the DXGI back buffer as a *borrowed* dx12_texture: DXGI owns the resource, so ~dx12_texture
        // drops our reference synchronously (the swapchain waits for the GPU before releasing). The render
        // pass creates the RTV on demand from the render_target_view acquire hands out, so the swapchain
        // keeps no RTV of its own.
        auto tex = std::make_shared<dx12_texture>(_ctx, _ctx.current_epoch(), td, cc::move(resource),
                                                  /*heap*/ nullptr, /*borrowed*/ true);
        _backbuffers.push_back(backbuffer{.texture = dx12_texture_handle(cc::move(tex)), .frame_fence_value = 0});
    }
    return cc::unit{};
}

void dx12_swapchain::wait_for_gpu()
{
    // Best-effort (never throws — it also runs from the destructor): a lost device never signals, and a
    // failed SetEventOnCompletion means the device is broken and we're tearing down anyway.
    if (_ctx.is_device_lost())
        return;
    if (_present_fence->GetCompletedValue() < _fence_value)
        if (SUCCEEDED(_present_fence->SetEventOnCompletion(_fence_value, _fence_event)))
            WaitForSingleObject(_fence_event, INFINITE);
}

void dx12_swapchain::release_backbuffers()
{
    _backbuffers.clear(); // borrowed → ~dx12_texture drops the DXGI reference synchronously (see wait_for_gpu)
}

cc::result<cc::unit> dx12_swapchain::resize(tg::vec2i size)
{
    // ResizeBuffers requires zero outstanding back-buffer references. Since acquire calls this at most once
    // per epoch, waiting for the GPU to finish every submitted present (the present fence) is enough to make
    // the back buffers safe to release — no epoch advance needed. Borrowed wrappers then release synchronously.
    wait_for_gpu();
    release_backbuffers();

    UINT const flags = _tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    if (HRESULT hr = _swapchain->ResizeBuffers(UINT(_desc.buffer_count), UINT(size[0]), UINT(size[1]),
                                               to_dxgi_format(_desc.format), flags);
        FAILED(hr))
        return dx12_error(hr, "IDXGISwapChain::ResizeBuffers failed");

    _size = size;
    return build_backbuffers();
}

sg::render_target_view dx12_swapchain::acquire_backbuffer()
{
    CC_ASSERT(!_acquired, "acquire_backbuffer() called twice without an intervening present()");

    // Auto-resize to the window — but only the first acquire of each epoch checks. That bounds resize (which
    // drains the GPU) to once per epoch, so it never advances an epoch under the caller.
    if (sg::epoch const epoch = _ctx.current_epoch(); epoch != _last_resize_epoch)
    {
        _last_resize_epoch = epoch;
        if (tg::vec2i const client = client_size_of(_hwnd); client != _size)
            if (auto r = resize(client); r.has_error())
                fail(DXGI_ERROR_DEVICE_REMOVED, "swapchain resize failed");
    }

    _acquired_index = _swapchain->GetCurrentBackBufferIndex();
    backbuffer const& bb = _backbuffers[_acquired_index];

    // Don't hand back a buffer whose previous frame is still in flight.
    if (_present_fence->GetCompletedValue() < bb.frame_fence_value)
    {
        if (HRESULT hr = _present_fence->SetEventOnCompletion(bb.frame_fence_value, _fence_event); FAILED(hr))
            fail(hr, "SetEventOnCompletion (swapchain present fence) failed");
        WaitForSingleObject(_fence_event, INFINITE);
    }

    _acquired = true;
    return sg::render_target_view(bb.texture, sg::texture_view_dimension::tex_2d, _desc.format, sg::subresource_range{});
}

void dx12_swapchain::record_present_transition(sg::command_list& cmd)
{
    CC_ASSERT(_acquired, "record_present_transition without a matching acquire_backbuffer()");
    auto* const dx = dynamic_cast<dx12_command_list*>(&cmd);
    CC_ASSERT(dx != nullptr, "command list is not a dx12 command list");

    // Transition the acquired back buffer to the PRESENT layout on the caller's still-open list. Going
    // through the barrier tracker computes it from whatever layout the frame left the buffer in (a no-op if
    // already present) and leaves its canonical layout as `present` for next frame.
    dx->transition_texture_to(_backbuffers[_acquired_index].texture, sg::texture_layout::present);
}

void dx12_swapchain::present()
{
    CC_ASSERT(_acquired, "present() without a matching acquire_backbuffer()");
    _acquired = false;

    UINT const sync_interval = _desc.present_mode == sg::present_mode::vsync ? 1u : 0u;
    UINT const flags = _tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u; // valid only with sync interval 0
    if (HRESULT hr = _swapchain->Present(sync_interval, flags); FAILED(hr))
        fail(hr, "IDXGISwapChain::Present failed");

    // Signal the present fence after the present is queued, so the next acquire of this index waits for it.
    ++_fence_value;
    if (HRESULT hr = _ctx._queue->Signal(_present_fence.Get(), _fence_value); FAILED(hr))
        fail(hr, "ID3D12CommandQueue::Signal (swapchain present fence) failed");
    _backbuffers[_acquired_index].frame_fence_value = _fence_value;
}

void dx12_swapchain::fail(HRESULT hr, char const* what)
{
    _ctx.note_device_removed_if_lost(hr, what);
    if (_ctx.is_device_lost())
        throw sg::device_lost_exception(_ctx.device_loss_reason());
    throw sg::exception(cc::format("{} (hr=0x{:08X})", what, cc::u32(hr)));
}

dx12_swapchain::~dx12_swapchain()
{
    // Wait for the GPU to finish with the back buffers, then release them synchronously (borrowed storage).
    wait_for_gpu();
    release_backbuffers();
    if (_fence_event != nullptr)
        CloseHandle(_fence_event);
}
} // namespace sg::backend::dx12
