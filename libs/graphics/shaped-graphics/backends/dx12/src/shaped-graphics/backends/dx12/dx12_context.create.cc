// dx12 context bring-up: debug layer, adapter selection, device + queue creation.

#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/string/conversion.hh> // utf16_to_utf8 — DXGI_ADAPTER_DESC1::Description is wide
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_dred.hh>

// ID3D12Debug / ID3D12InfoQueue1, the debug-layer interfaces, live in the SDK-layers header, separate from d3d12.h.
#include <d3d12sdklayers.h>

namespace sg::backend::dx12
{
namespace
{
/// Whether an HRESULT says the GPU went away rather than that it cannot do what was asked.
///
/// The distinction decides whether looking at the NEXT adapter is sensible: an adapter that does not support
/// feature level 11_0 is simply not a candidate, and one that just reset is a machine in trouble.
[[nodiscard]] bool is_device_loss_hresult(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG;
}

/// What searching for a hardware adapter found.
enum class adapter_search
{
    found,       ///< `out` holds it
    none,        ///< no hardware adapter supports D3D12 here
    device_lost, ///< one reported a reset/removal, and that is NOT a reason to go and use a different GPU
};

struct adapter_search_result
{
    adapter_search status = adapter_search::none;
    HRESULT device_loss_hr = S_OK;
};

/// The first hardware adapter that supports D3D12, into `out`.
/// WARP is skipped here, so falling back to it is always the caller's explicit choice.
///
/// **A device-loss HRESULT stops the search rather than skipping the adapter.**
/// Treating a reset GPU as "unsuitable" and quietly moving to the next one migrates the whole process to a
/// different physical device, which nobody asked for and which surfaces much later as something unrelated.
/// A reset is what `is_device_lost()` exists to report, so it is reported.
adapter_search_result find_hardware_adapter(IDXGIFactory4* factory, ComPtr<IDXGIAdapter1>& out)
{
    for (UINT i = 0; factory->EnumAdapters1(i, out.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 ad = {};
        out->GetDesc1(&ad);
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        // A null out-param probes D3D12 support (FL 11_0) without creating a device.
        HRESULT const hr = D3D12CreateDevice(out.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr);
        if (SUCCEEDED(hr))
            return {.status = adapter_search::found};

        if (is_device_loss_hresult(hr))
        {
            out = nullptr;
            return {.status = adapter_search::device_lost, .device_loss_hr = hr};
        }
    }
    out = nullptr;
    return {.status = adapter_search::none};
}

/// What `SC_DX12_ADAPTER` asks of this process.
enum class adapter_pin : u8
{
    none,
    hardware,
    warp,
    /// Both hidden: a host with no dx12 adapter at all, which is what a GPU-less CI job without WARP is.
    nothing,
};

/// Read on every call rather than once, so a test that sets the variable sees it take effect.
/// An unrecognized value is ignored, and said so once per process.
adapter_pin read_adapter_pin()
{
    auto const value = cc::environment_variable("SC_DX12_ADAPTER");
    if (!value.has_value())
        return adapter_pin::none;
    if (value.value() == "warp")
        return adapter_pin::warp;
    if (value.value() == "hardware")
        return adapter_pin::hardware;
    if (value.value() == "none")
        return adapter_pin::nothing;

    static auto warned = cc::atomic_flag();
    if (!warned.test_and_set())
        CC_LOG_WARNING("SC_DX12_ADAPTER='{}' is ignored: the accepted values are 'hardware', 'warp' and 'none'",
                       value.value());
    return adapter_pin::none;
}

/// What DXGI says about the adapter that was picked.
///
/// The driver version comes from CheckInterfaceSupport, which is the only place d3d12 exposes one at all.
/// It fails on WARP and on some drivers, and the empty string it then leaves correctly reads as "unknown" rather than
/// as a version anyone should compare.
sg::adapter_info describe_adapter(IDXGIAdapter1* adapter)
{
    auto info = sg::adapter_info();

    DXGI_ADAPTER_DESC1 desc = {};
    if (FAILED(adapter->GetDesc1(&desc)))
        return info;

    // Description is a fixed-size WCHAR buffer, NUL-terminated within it rather than length-prefixed.
    auto const capacity = isize(sizeof(desc.Description) / sizeof(desc.Description[0]));
    auto length = isize(0);
    while (length < capacity && desc.Description[length] != L'\0')
        ++length;
    info.name = cc::utf16_to_utf8(cc::span<char16_t const>(reinterpret_cast<char16_t const*>(desc.Description), length));

    info.vendor_id = u32(desc.VendorId);
    info.device_id = u32(desc.DeviceId);
    info.is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

    // What the board has, which is NOT the budget this process gets — see adapter_info.
    // Zero is a real answer for an integrated GPU, so it is reported rather than treated as absent.
    info.dedicated_video_memory_bytes = i64(desc.DedicatedVideoMemory);

    LARGE_INTEGER umd = {};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        info.driver_version = cc::format("{}.{}.{}.{}", u16(umd.HighPart >> 16), u16(umd.HighPart & 0xFFFF),
                                         u16(umd.LowPart >> 16), u16(umd.LowPart & 0xFFFF));

    return info;
}

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

/// Relays one debug-layer message at the level the debug layer itself assigned it.
///
/// Mapping rather than flattening: a corruption message and an info message are not the same news, and a reader
/// filtering to warnings and above is exactly who needs the difference.
void log_debug_layer_message(dx12_message_severity severity, char const* description)
{
    switch (severity)
    {
    case dx12_message_severity::corruption:
    case dx12_message_severity::error:
        CC_LOG_ERROR("debug layer: {}", description);
        break;
    case dx12_message_severity::warning:
        CC_LOG_WARNING("debug layer: {}", description);
        break;
    default:
        CC_LOG_INFO("debug layer: {}", description);
        break;
    }
}

// Every context whose callback is registered, oldest first.
//
// D3D12 hands one debug-layer message to every callback registered on the DEVICE that raised it, and hands two contexts
// on one adapter the same device — so a message logged by each context without a listener would print once per context.
// Only per device, though: a WARP context and a hardware context never see each other's messages.
// So the oldest context without a listener ON THE SAME DEVICE is the one that logs, which makes it once per device.
cc::mutex<cc::vector<dx12_context const*>> g_registered_contexts;

// Validation messages, handed to the context's listener or logged at the debug layer's own severity when it has none.
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
    {
        ctx->_message_callback(level, description);
        return;
    }

    if (ctx == nullptr)
    {
        log_debug_layer_message(level, description);
        return;
    }

    auto const is_logger = g_registered_contexts.lock(
        [&](cc::vector<dx12_context const*>& contexts)
        {
            for (auto const* const c : contexts)
                if (c->_device.Get() == ctx->_device.Get() && !c->_message_callback.is_valid())
                    return c == ctx;
            return false;
        });
    if (is_logger)
        log_debug_layer_message(level, description);
}

// Whether the one-shot below has RUN, whatever it found.
// Deliberately not the same question as whether the layer is on: a host without the Graphics Tools feature has nothing
// to activate, and must not look like a process that could still activate one -- otherwise the best-effort miss turns
// into a hard refusal for every later context.
cc::atomic<bool>& debug_layer_activation_attempted()
{
    static cc::atomic<bool> attempted = false;
    return attempted;
}

// Whether the layer is actually validating this process, which is the question the message callback asks.
cc::atomic<bool>& debug_layer_active()
{
    static cc::atomic<bool> active = false;
    return active;
}

// Whether this process has brought up a D3D12 device yet.
// Set once a device is created, WARP included -- the debug layer is process-wide, so a software device closes the window just as a hardware one does.
cc::atomic<bool>& process_has_device()
{
    static cc::atomic<bool> has_device = false;
    return has_device;
}
// Activates the D3D12 debug layer for the whole process, at most once, and reports whether it is available.
//
// EnableDebugLayer is a PROCESS-wide switch rather than a per-device one, so calling it per context creation is both redundant and unsafe:
// with several contexts coming up at once, one thread flipping it while another is inside CreateDXGIFactory2 makes that call fail with DXGI_ERROR_INVALID_CALL.
// A function-local static gives thread-safe once-only initialization and hands every later caller the same answer.
//
// It must also precede this process's FIRST DEVICE, which is the constraint with teeth.
// Arming the layer once a device exists does not merely fail to validate it: on an NVIDIA driver it RESETS the adapter,
// and every D3D12CreateDevice probe on that adapter then returns DXGI_ERROR_DEVICE_RESET while the GPU itself is fine.
// A process that wants validation therefore has to ask for it on the first context it creates, which is what debug_layer_activation_attempted guards below.
//
// One-way by construction, and deliberately so: a context created earlier may still be relying on the layer, so nothing here ever turns it back off.
//
// Best-effort: the layer needs the "Graphics Tools" feature, and a host without it runs unvalidated rather than failing to create a context.
bool activate_global_debug_layer_once()
{
    static bool const active = []
    {
        // Marked before anything can fail, and before any device exists: this records that the process has had its
        // one chance to activate, which is what the late-activation guard tests.
        debug_layer_activation_attempted() = true;

        ComPtr<ID3D12Debug> debug;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            return false;
        debug->EnableDebugLayer();
        debug_layer_active() = true;
        return true;
    }();
    return active;
}

// Routes D3D12 validation messages to dx12_message_callback, with `ctx` as the listener to consult.
// Registered once the context object exists, so anything the runtime raises during creation still takes the stderr path.
// Best-effort: needs ID3D12InfoQueue1, and is silently skipped when the interface isn't available.
u32 register_debug_callback(ID3D12Device* device, dx12_context* ctx)
{
    ComPtr<ID3D12InfoQueue1> info_queue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&info_queue))))
        return 0;

    // Listed before the registration, so no message can reach this context's callback while it is missing from the list.
    g_registered_contexts.lock([&](cc::vector<dx12_context const*>& contexts) { contexts.push_back(ctx); });

    DWORD cookie = 0;
    if (FAILED(info_queue->RegisterMessageCallback(&dx12_message_callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, ctx,
                                                   &cookie)))
    {
        g_registered_contexts.lock([&](cc::vector<dx12_context const*>& contexts) { contexts.remove_first_value(ctx); });
        return 0;
    }
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

    // After the runtime stopped calling it, and never under the lock across the unregister, which may wait on a callback in flight.
    g_registered_contexts.lock([&](cc::vector<dx12_context const*>& contexts) { contexts.remove_first_value(this); });
}
} // namespace sg::backend::dx12

bool sg::backend::dx12::has_hardware_adapter()
{
    if (auto const pin = read_adapter_pin(); pin == adapter_pin::warp || pin == adapter_pin::nothing)
        return false;

    static bool const has = []
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
            return false;
        ComPtr<IDXGIAdapter1> adapter;
        // A reset adapter is still a hardware adapter: answering `false` here would quietly hand the run to
        // WARP, which is the same substitution this function's caller is trying to avoid.
        return find_hardware_adapter(factory.Get(), adapter).status != adapter_search::none;
    }();
    return has;
}

namespace sg
{
cc::result<context_handle> create_dx12_context(backend::dx12::dx12_config const& config)
{
    // Adapter selection, device creation and every resource system's setup — tens of milliseconds, and the first
    // thing anyone looks at when a program is slow to show a window.
    CC_RECORD_SCOPE("sg.context.create");

    // Held across the whole creation, including a failed one destroying its half-built context; see impl/device_lifecycle.hh.
    sg::impl::device_lifecycle_hold const lifecycle;

    using namespace sg::backend::dx12;

    // No DXGI_CREATE_FACTORY_DEBUG here, deliberately.
    // That flag turns on DXGI's OWN message queue, which nothing in sg reads — validation reaches us through the device's ID3D12InfoQueue1 instead (see register_debug_callback).
    // It also makes concurrent context creation fail: CreateDXGIFactory2 with it set intermittently returns DXGI_ERROR_INVALID_CALL when several threads are in there at once.
    // So it was pure cost.
    UINT const factory_flags = 0;
    // Refused rather than done anyway: activating the layer now would reset the adapter, and that would surface
    // later as a GPU looking broken to every context this process creates afterwards.
    if (config.activate_global_debug_layer && !debug_layer_activation_attempted() && process_has_device())
    {
        // Only refused when there is something to refuse.
        // Asking whether the layer exists activates nothing, and a host without the Graphics Tools feature has no late
        // activation to perform -- so the request is the documented best-effort miss rather than the hazard, and it
        // must not fail a context that would simply have run unvalidated.
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            return dx12_error(E_INVALIDARG, "the dx12 debug layer must be activated before this process creates its "
                                            "first device; ask for it on the first context instead");
    }

    if (config.activate_global_debug_layer)
        activate_global_debug_layer_once();

    // Before the device, and that is the whole constraint: the runtime decides at creation whether to carry the
    // bookkeeping, so arming it afterwards records nothing.
    if (config.enable_dred)
        enable_dred_once();

    ComPtr<IDXGIFactory4> factory;
    if (HRESULT hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)); FAILED(hr))
        return dx12_error(hr, "CreateDXGIFactory2 failed");

    auto const pin = read_adapter_pin();
    auto choice = config.adapter;
    if (choice == dx12_adapter::hardware_or_warp && pin == adapter_pin::hardware)
        choice = dx12_adapter::hardware;

    // The warp pin hides hardware from every request, an explicit `hardware` included, not only from hardware_or_warp.
    auto const hardware_hidden = pin == adapter_pin::warp || pin == adapter_pin::nothing;
    ComPtr<IDXGIAdapter1> adapter;
    auto const search = hardware_hidden ? adapter_search_result{} : find_hardware_adapter(factory.Get(), adapter);

    // Reported rather than worked around: the machine has a GPU that just reset, and creating this context on
    // a different one would hide that behind whatever goes wrong next.
    if (search.status == adapter_search::device_lost)
        return dx12_error(search.device_loss_hr, "the hardware adapter reports a device reset or removal; "
                                                 "something already running on this GPU took it down");

    if (choice != dx12_adapter::warp && search.status != adapter_search::found)
    {
        if (choice == dx12_adapter::hardware && hardware_hidden)
            return cc::error(cc::format("no Direct3D 12 capable hardware adapter found (SC_DX12_ADAPTER={} hides them)",
                                        pin == adapter_pin::warp ? "warp" : "none"));
        if (choice == dx12_adapter::hardware)
            return cc::error("no Direct3D 12 capable hardware adapter found");
        choice = dx12_adapter::warp;
    }
    if (choice == dx12_adapter::warp)
    {
        // The hardware pin hides WARP from every request in turn, an explicit `warp` included.
        if (pin == adapter_pin::hardware || pin == adapter_pin::nothing)
            return cc::error(cc::format("no WARP adapter (SC_DX12_ADAPTER={} hides it)",
                                        pin == adapter_pin::hardware ? "hardware" : "none"));
        if (HRESULT hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)); FAILED(hr))
            return dx12_error(hr, "IDXGIFactory4::EnumWarpAdapter failed");
    }

    ComPtr<ID3D12Device> device;
    if (HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)); FAILED(hr))
        return dx12_error(hr, "D3D12CreateDevice failed");
    process_has_device() = true;

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
    ctx->set_adapter_info(describe_adapter(adapter.Get()));
    // IDXGIAdapter3 is where QueryVideoMemoryInfo lives; an older runtime leaves this null and the query then refuses.
    adapter.As(&ctx->_adapter);

    ctx->_factory = cc::move(factory);
    ctx->_device = cc::move(device);
    ctx->_queue = cc::move(queue);
    ctx->_raytracing_tier = raytracing_tier;
    ctx->_epoch_fence = cc::move(epoch_fence);
    ctx->_submission_fence = cc::move(submission_fence);

    // The completion signal waiter's three events; see the members.
    for (HANDLE* const event :
         {&ctx->_completion_submission_event, &ctx->_completion_epoch_event, &ctx->_completion_wake_event})
    {
        *event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (*event == nullptr)
            return cc::error("CreateEventW failed for a completion signal event");
    }

    // With the debug layer live, route validation messages through the context, so a listener can be set on it later.
    // Registered here rather than right after device creation: the callback needs the context to consult.
    //
    // Keyed on the layer actually being active, NOT on this context having asked for it: a context created after
    // something else activated it is validated all the same, and its messages belong on a listener rather than stderr.
    if (debug_layer_active())
        ctx->_message_callback_cookie = register_debug_callback(ctx->_device.Get(), ctx.get());

    // Completion timelines first: every copyable resource takes its groups from this pool at construction, so it
    // has to be live before anything can be created.
    ctx->_group_pool.initialize(ctx->_device.Get());

    // Bring up the inline transfer ring buffers; each system creates + maps its own heap (colocated
    // with its logic) off the now-populated device.
    ctx->_execution = config.execution;
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
