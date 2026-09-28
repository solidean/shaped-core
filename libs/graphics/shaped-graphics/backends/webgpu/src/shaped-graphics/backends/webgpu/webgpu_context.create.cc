// Bring-up: requesting an adapter and a device, and wrapping a device into a context.

#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/thread.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>

namespace sg::backend::webgpu
{
namespace
{
/// The optional features a context asks for wherever the adapter offers them.
constexpr WGPUFeatureName k_optional_features[] = {
    WGPUFeatureName_TimestampQuery,    WGPUFeatureName_TextureCompressionBC, WGPUFeatureName_Depth32FloatStencil8,
    WGPUFeatureName_DepthClipControl,  WGPUFeatureName_TextureFormatsTier1,  WGPUFeatureName_TextureFormatsTier2,
    WGPUFeatureName_Float32Filterable, WGPUFeatureName_BGRA8UnormStorage,
};

void on_uncaptured_error(WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void* userdata1, void*)
{
    auto const& anchor = *static_cast<std::shared_ptr<webgpu_callback_anchor>*>(userdata1);
    auto const text = from_wgpu(message);
    if (anchor->on_error)
        anchor->on_error(type, text);
    if (anchor->ctx == nullptr)
    {
        CC_LOG_ERROR("webgpu error after its context shut down: {}", text);
        return;
    }
    auto const kind = type == WGPUErrorType_OutOfMemory ? sg::device_error_kind::creation_failed
                                                        : sg::device_error_kind::validation;
    anchor->ctx->report_device_error({.kind = kind, .message = text});
}

void on_device_lost(WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView message, void* userdata1, void*)
{
    // Called exactly once per device, and last, so this is where the anchor both callbacks share is released.
    auto const anchor = std::unique_ptr<std::shared_ptr<webgpu_callback_anchor>>(
        static_cast<std::shared_ptr<webgpu_callback_anchor>*>(userdata1));
    auto* const ctx = (*anchor)->ctx;
    if (ctx == nullptr || reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_CallbackCancelled)
        return;
    ctx->mark_device_lost(from_wgpu(message));
    ctx->settle_due_completions();
}

/// Reads the adapter and limits a device reports into `ctx`, and starts its systems.
void finish_creation(webgpu_context& ctx)
{
    auto info = WGPUAdapterInfo{};
    if (wgpuDeviceGetAdapterInfo(ctx.device(), &info) == WGPUStatus_Success)
    {
        auto adapter = sg::adapter_info();
        adapter.name = from_wgpu(info.description);
        if (adapter.name.empty())
            adapter.name = from_wgpu(info.device);
        if (adapter.name.empty())
            adapter.name = from_wgpu(info.vendor);
        adapter.vendor_id = info.vendorID;
        adapter.device_id = info.deviceID;
        adapter.is_software = info.adapterType == WGPUAdapterType_CPU;
        ctx.set_adapter_info(cc::move(adapter));
        wgpuAdapterInfoFreeMembers(info);
    }

    auto limits = WGPULimits{};
    auto alignment = isize(256);
    if (wgpuDeviceGetLimits(ctx.device(), &limits) == WGPUStatus_Success && limits.minUniformBufferOffsetAlignment > 0)
        alignment = isize(limits.minUniformBufferOffsetAlignment);

    auto const has = [&](WGPUFeatureName f) { return wgpuDeviceHasFeature(ctx.device(), f) != WGPU_FALSE; };
    ctx.set_limits(alignment, {.timestamps = has(WGPUFeatureName_TimestampQuery),
                               .readwrite_storage_formats = has(WGPUFeatureName_TextureFormatsTier2),
                               .float32_filtering = has(WGPUFeatureName_Float32Filterable),
                               // sg's extended set holds bgra8_unorm, which WebGPU grants with a feature of its own.
                               .extended_storage_formats
                               = has(WGPUFeatureName_TextureFormatsTier1) && has(WGPUFeatureName_BGRA8UnormStorage)});
}

struct request_state
{
    std::shared_ptr<webgpu_callback_anchor> anchor;
    wgpu_instance instance;
    wgpu_adapter adapter;
    webgpu_config config;
    cc::shared_async<sg::context_handle> node;
    cc::vector<WGPUFeatureName> features;
};

void fail(request_state& state, cc::string why)
{
    state.node->push_error(cc::async_error::make_error(cc::any_error(cc::move(why))));
}

void on_device(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* userdata1, void*)
{
    auto const state = std::unique_ptr<request_state>(static_cast<request_state*>(userdata1));
    auto owned = wgpu_device(device);
    if (status != WGPURequestDeviceStatus_Success || !owned)
    {
        fail(*state, cc::format("no webgpu device: {}", from_wgpu(message)));
        return;
    }

    auto ctx = std::make_shared<webgpu_context>(cc::move(state->instance), cc::move(state->adapter), cc::move(owned),
                                                state->anchor, state->config);
    finish_creation(*ctx);
    state->node->push_value(sg::context_handle(cc::move(ctx)));
}

void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* userdata1, void*)
{
    auto state = std::unique_ptr<request_state>(static_cast<request_state*>(userdata1));
    state->adapter = wgpu_adapter(adapter);
    if (status != WGPURequestAdapterStatus_Success || !state->adapter)
    {
        fail(*state, cc::format("no webgpu adapter: {}", from_wgpu(message)));
        return;
    }

    for (auto const f : k_optional_features)
        if (wgpuAdapterHasFeature(state->adapter.get(), f) != WGPU_FALSE)
            state->features.push_back(f);

    // Both callbacks share one anchor handle, released by the device-lost callback, which is always the last.
    auto* const anchor_handle = new std::shared_ptr<webgpu_callback_anchor>(state->anchor);

    auto desc = WGPUDeviceDescriptor{};
    desc.label = to_wgpu("sg device");
    desc.requiredFeatureCount = size_t(state->features.size());
    desc.requiredFeatures = state->features.empty() ? nullptr : state->features.data();
    desc.requiredLimits = nullptr; // the defaults, which is what every portable sg caller sizes against
    desc.defaultQueue.label = to_wgpu("sg queue");
    desc.deviceLostCallbackInfo = WGPUDeviceLostCallbackInfo{
        .nextInChain = nullptr,
        .mode = WGPUCallbackMode_AllowSpontaneous,
        .callback = on_device_lost,
        .userdata1 = anchor_handle,
        .userdata2 = nullptr,
    };
    desc.uncapturedErrorCallbackInfo = WGPUUncapturedErrorCallbackInfo{
        .nextInChain = nullptr,
        .callback = on_uncaptured_error,
        .userdata1 = anchor_handle,
        .userdata2 = nullptr,
    };

    auto const raw_adapter = state->adapter.get();
    auto const info = WGPURequestDeviceCallbackInfo{
        .nextInChain = nullptr,
        .mode = WGPUCallbackMode_AllowSpontaneous,
        .callback = on_device,
        .userdata1 = state.release(),
        .userdata2 = nullptr,
    };
    (void)wgpuAdapterRequestDevice(raw_adapter, &desc, info);
}

/// The request made from main, for a caller on another thread: the device lives where it was requested.
[[nodiscard]] cc::shared_async<sg::context_handle> request_on_main(webgpu_config config)
{
    co_await cc::async_resume_on_main();
    auto const requested = sg::request_webgpu_context(config);
    co_return co_await requested;
}
} // namespace
} // namespace sg::backend::webgpu

cc::shared_async<sg::context_handle> sg::request_webgpu_context(backend::webgpu::webgpu_config const& config)
{
    namespace webgpu = backend::webgpu;

    if (cc::current_thread_id() != cc::thread_id::main)
        return webgpu::request_on_main(config);

    auto instance = webgpu::wgpu_instance(wgpuCreateInstance(nullptr));
    if (!instance)
        return cc::make_async_from_error<context_handle>(cc::async_error::make_error(cc::any_error("this runtime has "
                                                                                                   "no webgpu")));

    auto state = std::make_unique<webgpu::request_state>();
    state->anchor = std::make_shared<webgpu::webgpu_callback_anchor>();
    state->config = config;
    state->node = cc::make_async_manual<context_handle>();
    auto node = state->node;

    auto options = WGPURequestAdapterOptions{};
    options.featureLevel = WGPUFeatureLevel_Core;
    options.powerPreference
        = config.prefer_low_power ? WGPUPowerPreference_LowPower : WGPUPowerPreference_HighPerformance;
    options.forceFallbackAdapter = WGPU_FALSE;

    auto const raw_instance = instance.get();
    state->instance = cc::move(instance);
    auto const info = WGPURequestAdapterCallbackInfo{
        .nextInChain = nullptr,
        .mode = WGPUCallbackMode_AllowSpontaneous,
        .callback = webgpu::on_adapter,
        .userdata1 = state.release(),
        .userdata2 = nullptr,
    };
    (void)wgpuInstanceRequestAdapter(raw_instance, &options, info);
    return node;
}

cc::result<sg::context_handle> sg::create_webgpu_context(WGPUDevice device, backend::webgpu::webgpu_config const& config)
{
    namespace webgpu = backend::webgpu;
    if (device == nullptr)
        return cc::error("create_webgpu_context needs a device");

    wgpuDeviceAddRef(device);
    auto ctx = std::make_shared<webgpu::webgpu_context>(webgpu::wgpu_instance(wgpuCreateInstance(nullptr)),
                                                        webgpu::wgpu_adapter(), webgpu::wgpu_device(device),
                                                        std::make_shared<webgpu::webgpu_callback_anchor>(), config);
    webgpu::finish_creation(*ctx);
    return context_handle(cc::move(ctx));
}
