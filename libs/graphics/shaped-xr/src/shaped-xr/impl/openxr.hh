#pragma once

// OpenXR with the bindings this build has, and the state behind xr::system and xr::session.
// Only shaped-xr's own sources include this: the public headers name no OpenXR, Vulkan or D3D12 type.

#include <shaped-xr/xr.hh>

#if XR_HAS_OPENXR

#if XR_HAS_VULKAN
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#if XR_HAS_DX12
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#define XR_USE_GRAPHICS_API_D3D12
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace xr::impl
{
/// `result` as a message naming `what`, or empty where it succeeded.
[[nodiscard]] cc::string failed(XrInstance instance, XrResult result, cc::string_view what);

/// An instance function the loader resolves by name, or null where the runtime lacks it.
template <class F>
[[nodiscard]] F proc(XrInstance instance, char const* name)
{
    auto f = PFN_xrVoidFunction(nullptr);
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &f)))
        return nullptr;
    return reinterpret_cast<F>(f);
}

/// One swapchain of `layers` array layers and the sg texture each of its images is rendered as or copied into.
struct swapchain
{
    XrSwapchain handle = XR_NULL_HANDLE;
    i64 format = 0;
    /// Per image: on vulkan the image itself, wrapped; empty on dx12, where `copied` is rendered into instead.
    cc::vector<sg::texture_2d_array> wrapped;
#if XR_HAS_DX12
    cc::vector<ID3D12Resource*> dx12_images;
#endif
    u32 acquired = 0;
};
} // namespace xr::impl

struct xr::system::state
{
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    graphics_api api = graphics_api::vulkan;
    cc::string runtime_name;
    cc::string system_name;
    bool offers_passthrough = false;
    /// Whether each view hands the runtime its depth too (`XR_KHR_composition_layer_depth`).
    bool offers_depth_layer = false;

    ~state();
};

struct xr::session::state
{
    system::state const* sys = nullptr;
    sg::context* ctx = nullptr;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    session_description desc;
    XrSessionState current = XR_SESSION_STATE_UNKNOWN;
    bool is_running = false;

    int width = 0;
    int height = 0;
    u32 view_count = 0;
    impl::swapchain color;
    impl::swapchain depth;
    /// On dx12, what the frame renders into before the copy into the runtime's images; the wrapped image on vulkan.
    sg::texture_2d_array own_color;
    sg::texture_2d_array own_depth;

    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthrough_layer = XR_NULL_HANDLE;

    // the frame in flight, between wait_frame and submit_frame
    bool in_frame = false;
    bool frame_renders = false;
    XrTime display_time = 0;
    cc::vector<XrView> views;

    ~state();
};

#endif
