#pragma once

// OpenXR with the bindings this build has, and the state behind sxr::system and sxr::session.
// Only shaped-xr's own sources include this: the public headers name no OpenXR, Vulkan or D3D12 type.

#include <shaped-xr/impl/capabilities.hh>
#include <shaped-xr/xr.hh>

#if SXR_HAS_OPENXR

#if SXR_HAS_VULKAN
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#if SXR_HAS_DX12
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#define XR_USE_GRAPHICS_API_D3D12
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace sxr::impl
{
/// `result` as a message naming `what`, or empty where it succeeded.
[[nodiscard]] cc::string failed(XrInstance instance, XrResult result, cc::string_view what);

/// The system's name and what it answers about the extensions in `enabled`, which were enabled on `instance`.
struct system_answer
{
    cc::string name;
    system_support support;
};
[[nodiscard]] cc::result<system_answer> read_system(XrInstance instance,
                                                    XrSystemId system,
                                                    cc::span<char const* const> enabled);

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
#if SXR_HAS_DX12
    cc::vector<ID3D12Resource*> dx12_images;
#endif
    u32 acquired = 0;
};
} // namespace sxr::impl

struct sxr::system::state
{
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    graphics_api api = graphics_api::vulkan;
    cc::string runtime_name;
    cc::string system_name;
    /// What the description wanted and the headset has.
    cc::vector<capability> offered;
    /// What the runtime composes by itself, as `xrEnumerateEnvironmentBlendModes` lists it.
    cc::vector<blend_mode> runtime_blends;
    /// Whether each view hands the runtime its depth too (`XR_KHR_composition_layer_depth`).
    bool offers_depth_layer = false;

    ~state();
};

struct sxr::session::state
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

    blend_mode blend = blend_mode::opaque;
    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthrough_layer = XR_NULL_HANDLE;

    // the actions, parallel to desc.actions; a pose action has a space and a located pose per hand, two slots each
    XrActionSet action_set = XR_NULL_HANDLE;
    cc::vector<XrAction> actions;
    XrPath hand_paths[2] = {XR_NULL_PATH, XR_NULL_PATH};
    cc::vector<XrSpace> pose_spaces;
    cc::vector<cc::optional<tg::rigid_transform3f>> poses;

    /// The position of `name` in desc.actions; asserts on a name the session was not created with.
    [[nodiscard]] isize action_index(cc::string_view name) const;

    // the frame in flight, between wait_frame and submit_frame
    bool in_frame = false;
    bool frame_renders = false;
    XrTime display_time = 0;
    cc::vector<XrView> views;

    ~state();
};

#endif
