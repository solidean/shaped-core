// dx12 context bring-up: debug layer, adapter selection, device + queue creation. Split off from the
// other dx12_context bodies because it is the heavy path and grows with every device feature we opt
// into (feature-level probing, tearing, GPU-based validation, ...).

#include <clean-core/string/print.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// ID3D12Debug / ID3D12InfoQueue1 (debug-layer interfaces) live in the SDK-layers header, separate
// from d3d12.h.
#include <d3d12sdklayers.h>

namespace sg::backend::dx12
{
namespace
{
// Validation messages routed to stderr. Registered on the device's info queue when the debug layer
// is active; runs on whatever thread the runtime raises the message from.
void CALLBACK dx12_message_callback(D3D12_MESSAGE_CATEGORY /*category*/,
                                    D3D12_MESSAGE_SEVERITY severity,
                                    D3D12_MESSAGE_ID /*id*/,
                                    LPCSTR description,
                                    void* /*context*/)
{
    char const* level = "message";
    switch (severity)
    {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        level = "corruption";
        break;
    case D3D12_MESSAGE_SEVERITY_ERROR:
        level = "error";
        break;
    case D3D12_MESSAGE_SEVERITY_WARNING:
        level = "warning";
        break;
    case D3D12_MESSAGE_SEVERITY_INFO:
        level = "info";
        break;
    case D3D12_MESSAGE_SEVERITY_MESSAGE:
        level = "message";
        break;
    }
    cc::eprintln("[dx12 {}] {}", level, description);
}

// Routes D3D12 validation messages to dx12_message_callback. Best-effort: needs ID3D12InfoQueue1
// (recent SDK/runtime); silently skipped when the interface isn't available.
void register_debug_callback(ID3D12Device* device)
{
    ComPtr<ID3D12InfoQueue1> info_queue;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info_queue))))
    {
        DWORD cookie = 0;
        info_queue->RegisterMessageCallback(&dx12_message_callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
    }
}
} // namespace
} // namespace sg::backend::dx12

namespace sg
{
cc::result<context_handle> create_dx12_context(backend::dx12::dx12_config const& config)
{
    using namespace sg::backend::dx12;

    UINT factory_flags = 0;
    if (config.enable_debug_layer)
    {
        // Best-effort: needs the "Graphics Tools" feature; skip validation if it's absent, don't fail.
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }

    ComPtr<IDXGIFactory4> factory;
    if (HRESULT hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)); FAILED(hr))
        return dx12_error(hr, "CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> adapter;
    if (config.use_warp)
    {
        if (HRESULT hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)); FAILED(hr))
            return dx12_error(hr, "IDXGIFactory4::EnumWarpAdapter failed");
    }
    else
    {
        bool found = false;
        for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 ad = {};
            adapter->GetDesc1(&ad);
            if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue; // WARP is opt-in via use_warp, not a silent fallback.

            // Null out-param probes D3D12 support (FL 11_0) without creating a device.
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            {
                found = true;
                break;
            }
        }
        if (!found)
            return cc::error("no Direct3D 12 capable hardware adapter found");
    }

    ComPtr<ID3D12Device> device;
    if (HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)); FAILED(hr))
        return dx12_error(hr, "D3D12CreateDevice failed");

    // With the debug layer live, mirror validation messages to stderr.
    if (config.enable_debug_layer)
        register_debug_callback(device.Get());

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (HRESULT hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateCommandQueue failed");

    // Epoch system fences (both timelines on the direct queue) + a reusable wait event. The epoch
    // fence gates resource reclamation; the submission fence tracks per-command-list completion.
    ComPtr<ID3D12Fence> epoch_fence;
    if (HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&epoch_fence)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateFence (epoch) failed");

    ComPtr<ID3D12Fence> submission_fence;
    if (HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&submission_fence)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateFence (submission) failed");

    // Query DXR support once; the raytracing build path gates on it (cmd.raytracing.is_supported()). A
    // failed query leaves the tier at NOT_SUPPORTED, which is the correct "no ray tracing" answer.
    D3D12_RAYTRACING_TIER raytracing_tier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
        raytracing_tier = options5.RaytracingTier;

    auto ctx = std::make_shared<dx12_context>();
    ctx->_factory = cc::move(factory);
    ctx->_device = cc::move(device);
    ctx->_queue = cc::move(queue);
    ctx->_raytracing_tier = raytracing_tier;
    ctx->_epoch_fence = cc::move(epoch_fence);
    ctx->_submission_fence = cc::move(submission_fence);

    // Bring up the inline transfer ring buffers; each system creates + maps its own heap (colocated
    // with its logic) off the now-populated device.
    CC_RETURN_IF_ERROR(ctx->_upload_inline.initialize(config.upload_ring_bytes));
    CC_RETURN_IF_ERROR(ctx->_download_inline.initialize(config.download_ring_bytes));

    // Async upload + download staging windows and copy actors; each creates its own copy queue +
    // completion fence.
    CC_RETURN_IF_ERROR(ctx->_upload_async.initialize(config.async_upload_window_bytes));
    CC_RETURN_IF_ERROR(ctx->_download_async.initialize(config.async_download_window_bytes));

    // The GPU-query heap pool; caches the direct queue's timestamp frequency (→ tick→seconds factor).
    CC_RETURN_IF_ERROR(ctx->_query_system.initialize());

    // The shader-visible descriptor heap binding_groups allocate their tables from. Split into a
    // per-epoch-reclaimed transient ring (leading fraction) and a persistent bump region (the rest).
    CC_RETURN_IF_ERROR(ctx->_descriptor_heap.initialize(*ctx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                        config.descriptor_heap_capacity,
                                                        config.descriptor_transient_fraction));

    // The separate shader-visible SAMPLER heap dynamic samplers are written into (same lifetime split).
    CC_RETURN_IF_ERROR(ctx->_sampler_heap.initialize(
        *ctx, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, config.sampler_heap_capacity, config.descriptor_transient_fraction));

    return context_handle(cc::move(ctx));
}
} // namespace sg
