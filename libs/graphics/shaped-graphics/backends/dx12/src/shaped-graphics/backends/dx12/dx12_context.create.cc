// dx12 context bring-up: debug layer, adapter selection, device + queue creation.

#include <clean-core/string/print.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

// ID3D12Debug / ID3D12InfoQueue1, the debug-layer interfaces, live in the SDK-layers header, separate from d3d12.h.
#include <d3d12sdklayers.h>

namespace sg::backend::dx12
{
namespace
{
dx12_message_severity to_sg_severity(D3D12_MESSAGE_SEVERITY severity)
{
    switch (severity)
    {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        return dx12_message_severity::corruption;
    case D3D12_MESSAGE_SEVERITY_ERROR:
        return dx12_message_severity::error;
    case D3D12_MESSAGE_SEVERITY_WARNING:
        return dx12_message_severity::warning;
    case D3D12_MESSAGE_SEVERITY_INFO:
        return dx12_message_severity::info;
    default:
        return dx12_message_severity::message;
    }
}

char const* severity_label(dx12_message_severity severity)
{
    switch (severity)
    {
    case dx12_message_severity::corruption:
        return "corruption";
    case dx12_message_severity::error:
        return "error";
    case dx12_message_severity::warning:
        return "warning";
    case dx12_message_severity::info:
        return "info";
    default:
        return "message";
    }
}

// Validation messages, handed to the context's listener or written to stderr when it has none.
// Registered on the device's info queue when the debug layer is active, and runs on whatever thread the runtime raises the message from.
void CALLBACK dx12_message_callback(D3D12_MESSAGE_CATEGORY /*category*/,
                                    D3D12_MESSAGE_SEVERITY severity,
                                    D3D12_MESSAGE_ID /*id*/,
                                    LPCSTR description,
                                    void* context)
{
    auto const level = to_sg_severity(severity);
    auto* const ctx = static_cast<dx12_context*>(context);
    if (ctx != nullptr && ctx->_message_callback.is_valid())
        ctx->_message_callback(level, description);
    else
        cc::eprintln("[dx12 {}] {}", severity_label(level), description);
}

// Turns the D3D12 debug layer on, at most once for the whole process, and reports whether it is available.
//
// EnableDebugLayer is a PROCESS-wide switch rather than a per-device one, so calling it per context creation is both redundant and unsafe:
// with several contexts coming up at once, one thread flipping it while another is inside CreateDXGIFactory2 makes that call fail with DXGI_ERROR_INVALID_CALL.
// A function-local static gives thread-safe once-only initialization and hands every later caller the same answer.
//
// Best-effort: the layer needs the "Graphics Tools" feature, and a host without it runs unvalidated rather than failing to create a context.
bool enable_debug_layer_once()
{
    static bool const enabled = []
    {
        ComPtr<ID3D12Debug> debug;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            return false;
        debug->EnableDebugLayer();
        return true;
    }();
    return enabled;
}

// Routes D3D12 validation messages to dx12_message_callback, with `ctx` as the listener to consult.
// Registered once the context object exists, so anything the runtime raises during creation still takes the stderr path.
// Best-effort: needs ID3D12InfoQueue1, and is silently skipped when the interface isn't available.
u32 register_debug_callback(ID3D12Device* device, dx12_context* ctx)
{
    ComPtr<ID3D12InfoQueue1> info_queue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&info_queue))))
        return 0;

    DWORD cookie = 0;
    if (FAILED(info_queue->RegisterMessageCallback(&dx12_message_callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, ctx,
                                                   &cookie)))
        return 0;
    return u32(cookie);
}
} // namespace

void dx12_context::unregister_message_callback()
{
    if (_message_callback_cookie == 0 || !_device)
        return;

    ComPtr<ID3D12InfoQueue1> info_queue;
    if (SUCCEEDED(_device->QueryInterface(IID_PPV_ARGS(&info_queue))))
        info_queue->UnregisterMessageCallback(DWORD(_message_callback_cookie));
    _message_callback_cookie = 0;
}
} // namespace sg::backend::dx12

namespace sg
{
cc::result<context_handle> create_dx12_context(backend::dx12::dx12_config const& config)
{
    using namespace sg::backend::dx12;

    // No DXGI_CREATE_FACTORY_DEBUG here, deliberately.
    // That flag turns on DXGI's OWN message queue, which nothing in sg reads — validation reaches us through the device's ID3D12InfoQueue1 instead (see register_debug_callback).
    // It also makes concurrent context creation fail: CreateDXGIFactory2 with it set intermittently returns DXGI_ERROR_INVALID_CALL when several threads are in there at once.
    // So it was pure cost.
    UINT const factory_flags = 0;
    if (config.enable_debug_layer)
        enable_debug_layer_once();

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

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (HRESULT hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateCommandQueue failed");

    // Epoch system fences, both timelines on the direct queue.
    // The epoch fence gates resource reclamation; the submission fence tracks per-command-list completion.
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

    // With the debug layer live, route validation messages through the context, so a listener can be set on it later.
    // Registered here rather than right after device creation: the callback needs the context to consult.
    if (config.enable_debug_layer)
        ctx->_message_callback_cookie = register_debug_callback(ctx->_device.Get(), ctx.get());

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

    // The shader-visible descriptor heap binding_groups allocate their tables from.
    // Split into a per-epoch-reclaimed transient ring (the leading fraction) and a persistent bump region (the rest).
    CC_RETURN_IF_ERROR(ctx->_descriptor_heap.initialize(*ctx, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                        config.descriptor_heap_capacity,
                                                        config.descriptor_transient_fraction));

    // The separate shader-visible SAMPLER heap dynamic samplers are written into (same lifetime split).
    CC_RETURN_IF_ERROR(ctx->_sampler_heap.initialize(
        *ctx, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, config.sampler_heap_capacity, config.descriptor_transient_fraction));

    // Non-shader-visible RTV / DSV heaps render-target / depth-stencil views are created into.
    auto rtv_heap = dx12_cpu_descriptor_heap::create(*ctx, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, config.rtv_heap_capacity);
    CC_RETURN_IF_ERROR(rtv_heap);
    ctx->_rtv_heap = cc::move(rtv_heap.value());
    auto dsv_heap = dx12_cpu_descriptor_heap::create(*ctx, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, config.dsv_heap_capacity);
    CC_RETURN_IF_ERROR(dsv_heap);
    ctx->_dsv_heap = cc::move(dsv_heap.value());

    return context_handle(cc::move(ctx));
}
} // namespace sg
