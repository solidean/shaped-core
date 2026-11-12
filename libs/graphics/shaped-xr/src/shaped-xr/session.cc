#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/all.hh>
#include <shaped-xr/impl/actions.hh>
#include <shaped-xr/impl/openxr.hh>
#include <shaped-xr/impl/view_math.hh>

#if SXR_HAS_DX12
#include <shaped-graphics/backends/dx12/dx12_native_scope.hh>

using namespace cc::primitive_defines;
#endif

sxr::session::session(cc::unique_ptr<state> s) : _state(cc::move(s))
{
}
sxr::session::~session() = default;

#if !SXR_HAS_OPENXR

struct sxr::session::state
{
};
bool sxr::session::poll_events()
{
    return false;
}
bool sxr::session::is_running() const
{
    return false;
}
bool sxr::session::is_focused() const
{
    return false;
}
sxr::blend_mode sxr::session::blend() const
{
    return blend_mode::opaque;
}
sxr::availability sxr::session::availability_of(capability) const
{
    return availability::unsupported;
}
cc::optional<sxr::frame> sxr::session::wait_frame()
{
    return {};
}
void sxr::session::submit_frame(std::unique_ptr<sg::command_list>)
{
}
bool sxr::session::pressed(cc::string_view, hand) const
{
    return false;
}
sxr::f32 sxr::session::scalar(cc::string_view, hand) const
{
    return 0.0f;
}
tg::vec2f sxr::session::vector2(cc::string_view, hand) const
{
    return {};
}
cc::optional<tg::rigid_transform3f> sxr::session::pose(cc::string_view, hand) const
{
    return {};
}
void sxr::session::vibrate(cc::string_view, hand, f32, f32)
{
}

#else

namespace
{
[[nodiscard]] cc::any_error error_of(cc::string message)
{
    return cc::any_error(cc::move(message));
}

/// The first of `wanted` the runtime offers, by its API's own numbers, or 0.
[[nodiscard]] i64 pick_format(XrSession session, cc::span<i64 const> wanted)
{
    auto count = u32(0);
    xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
    auto offered = cc::vector<i64>::create_defaulted(count);
    xrEnumerateSwapchainFormats(session, count, &count, offered.data());
    for (auto const w : wanted)
        for (auto const o : offered)
            if (o == w)
                return w;
    return 0;
}

/// What sg calls a swapchain format, among the ones `pick_format` is offered.
[[nodiscard]] sg::pixel_format pixel_format_of(sxr::graphics_api api, i64 format)
{
#if SXR_HAS_VULKAN
    if (api == sxr::graphics_api::vulkan)
        switch (VkFormat(format))
        {
        case VK_FORMAT_R8G8B8A8_SRGB:
            return sg::pixel_format::rgba8_unorm_srgb;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return sg::pixel_format::bgra8_unorm_srgb;
        case VK_FORMAT_D32_SFLOAT:
            return sg::pixel_format::depth32_float;
        default:
            return sg::pixel_format::undefined;
        }
#endif
#if SXR_HAS_DX12
    if (api == sxr::graphics_api::dx12)
        switch (DXGI_FORMAT(format))
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return sg::pixel_format::rgba8_unorm_srgb;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return sg::pixel_format::bgra8_unorm_srgb;
        case DXGI_FORMAT_D32_FLOAT:
            return sg::pixel_format::depth32_float;
        default:
            return sg::pixel_format::undefined;
        }
#endif
    return sg::pixel_format::undefined;
}
} // namespace

sxr::session::state::~state()
{
    auto const destroy_layer
        = impl::proc<PFN_xrDestroyPassthroughLayerFB>(sys->instance, "xrDestroyPassthroughLayerFB");
    auto const destroy_passthrough = impl::proc<PFN_xrDestroyPassthroughFB>(sys->instance, "xrDestroyPassthroughFB");
    if (passthrough_layer != XR_NULL_HANDLE && destroy_layer != nullptr)
        destroy_layer(passthrough_layer);
    if (passthrough != XR_NULL_HANDLE && destroy_passthrough != nullptr)
        destroy_passthrough(passthrough);
    for (auto const space : pose_spaces)
        if (space != XR_NULL_HANDLE)
            xrDestroySpace(space);
    if (action_set != XR_NULL_HANDLE)
        xrDestroyActionSet(action_set); // and every action in it
    // the wrapped textures go before the swapchains whose images they name
    color.wrapped.clear();
    depth.wrapped.clear();
    if (color.handle != XR_NULL_HANDLE)
        xrDestroySwapchain(color.handle);
    if (depth.handle != XR_NULL_HANDLE)
        xrDestroySwapchain(depth.handle);
    if (space != XR_NULL_HANDLE)
        xrDestroySpace(space);
    if (session != XR_NULL_HANDLE)
        xrDestroySession(session);
}

cc::result<cc::unique_ptr<sxr::session>> sxr::system::create_session(sg::context& ctx, session_description const& desc)
{
    auto s = cc::make_unique<session::state>();
    s->sys = _state.get();
    s->ctx = &ctx;
    s->desc = desc;
    auto const instance = _state->instance;

    // ---- the session over the context's device and queue
#if SXR_HAS_VULKAN
    auto vk_binding = XrGraphicsBindingVulkan2KHR{.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    if (_state->api == graphics_api::vulkan)
    {
        auto* const vk = dynamic_cast<sg::backend::vulkan::vulkan_context*>(&ctx);
        if (vk == nullptr)
            return cc::error(error_of("a vulkan system's session needs a vulkan context, from system::create_context"));
        vk_binding.instance = vk->instance();
        vk_binding.physicalDevice = vk->physical_device();
        vk_binding.device = vk->device();
        vk_binding.queueFamilyIndex = vk->queue_family_index();
        vk_binding.queueIndex = 0;
    }
#endif
#if SXR_HAS_DX12
    auto dx_binding = XrGraphicsBindingD3D12KHR{.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    if (_state->api == graphics_api::dx12)
    {
        auto* const dx = dynamic_cast<sg::backend::dx12::dx12_context*>(&ctx);
        if (dx == nullptr)
            return cc::error(error_of("a dx12 system's session needs a dx12 context, from system::create_context"));
        dx_binding.device = dx->device();
        dx_binding.queue = dx->queue();
    }
#endif
    void const* binding = nullptr;
#if SXR_HAS_VULKAN
    if (_state->api == graphics_api::vulkan)
        binding = &vk_binding;
#endif
#if SXR_HAS_DX12
    if (_state->api == graphics_api::dx12)
        binding = &dx_binding;
#endif
    auto const session_info
        = XrSessionCreateInfo{.type = XR_TYPE_SESSION_CREATE_INFO, .next = binding, .systemId = _state->system};
    if (auto const why = impl::failed(instance, xrCreateSession(instance, &session_info, &s->session), "xrCreateSession");
        !why.empty())
        return cc::error(error_of(why));
    auto const space_info = XrReferenceSpaceCreateInfo{.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
                                                       .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                       .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}}};
    if (auto const why
        = impl::failed(instance, xrCreateReferenceSpace(s->session, &space_info, &s->space), "xrCreateReferenceSpace");
        !why.empty())
        return cc::error(error_of(why));

    // ---- the actions, bound before the session begins, since OpenXR attaches them once
    if (!desc.actions.empty())
    {
        if (auto const why = impl::problem_of(desc.actions); !why.empty())
            return cc::error(error_of(why));
        auto const path = [&](cc::string_view text)
        {
            auto p = XrPath(XR_NULL_PATH);
            auto copy = cc::string(text);
            xrStringToPath(instance, copy.c_str_materialize(), &p);
            return p;
        };
        s->hand_paths[0] = path("/user/hand/left");
        s->hand_paths[1] = path("/user/hand/right");
        auto const copy_name = [](char* to, isize capacity, cc::string_view from)
        {
            auto const n = cc::min(from.size(), capacity - 1);
            for (auto i = isize(0); i < n; ++i)
                to[i] = from[i];
            to[n] = 0;
        };
        auto set_info = XrActionSetCreateInfo{.type = XR_TYPE_ACTION_SET_CREATE_INFO, .priority = 0};
        copy_name(set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "shaped_xr");
        copy_name(set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Application");
        if (auto const why
            = impl::failed(instance, xrCreateActionSet(instance, &set_info, &s->action_set), "xrCreateActionSet");
            !why.empty())
            return cc::error(error_of(why));
        for (auto const& a : desc.actions)
        {
            auto info = XrActionCreateInfo{.type = XR_TYPE_ACTION_CREATE_INFO,
                                           .countSubactionPaths = a.is_per_hand ? 2u : 0u,
                                           .subactionPaths = a.is_per_hand ? s->hand_paths : nullptr};
            switch (a.kind)
            {
            case action_kind::boolean:
                info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
                break;
            case action_kind::scalar:
                info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
                break;
            case action_kind::vector2:
                info.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
                break;
            case action_kind::pose:
                info.actionType = XR_ACTION_TYPE_POSE_INPUT;
                break;
            case action_kind::vibration:
                info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
                break;
            }
            copy_name(info.actionName, XR_MAX_ACTION_NAME_SIZE, a.name);
            copy_name(info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE,
                      a.label.empty() ? cc::string_view(a.name) : cc::string_view(a.label));
            auto action = XrAction(XR_NULL_HANDLE);
            if (auto const why = impl::failed(instance, xrCreateAction(s->action_set, &info, &action),
                                              cc::format("xrCreateAction {}", a.name));
                !why.empty())
                return cc::error(error_of(why));
            s->actions.push_back(action);
        }
        // a profile the runtime does not know is refused alone, and the others still bind
        for (auto const& group : impl::bindings_by_profile(desc.actions))
        {
            auto suggested = cc::vector<XrActionSuggestedBinding>();
            for (auto i = isize(0); i < group.actions.size(); ++i)
                suggested.push_back({.action = s->actions[group.actions[i]], .binding = path(group.paths[i])});
            auto const info = XrInteractionProfileSuggestedBinding{.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
                                                                   .interactionProfile = path(group.profile),
                                                                   .countSuggestedBindings = u32(suggested.size()),
                                                                   .suggestedBindings = suggested.data()};
            if (auto const why = impl::failed(instance, xrSuggestInteractionProfileBindings(instance, &info),
                                              "xrSuggestInteractionProfileBindings");
                !why.empty())
                CC_LOG_WARNING("{} for {}", why, group.profile);
        }
        auto const attach = XrSessionActionSetsAttachInfo{.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
                                                          .countActionSets = 1,
                                                          .actionSets = &s->action_set};
        if (auto const why
            = impl::failed(instance, xrAttachSessionActionSets(s->session, &attach), "xrAttachSessionActionSets");
            !why.empty())
            return cc::error(error_of(why));
        for (auto i = isize(0); i < desc.actions.size(); ++i)
            for (auto h = 0; h < 2; ++h)
            {
                auto space = XrSpace(XR_NULL_HANDLE);
                if (desc.actions[i].kind == action_kind::pose && (desc.actions[i].is_per_hand || h == 0))
                {
                    auto const info = XrActionSpaceCreateInfo{
                        .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
                        .action = s->actions[i],
                        .subactionPath = desc.actions[i].is_per_hand ? s->hand_paths[h] : XR_NULL_PATH,
                        .poseInActionSpace = {.orientation = {0, 0, 0, 1}}};
                    xrCreateActionSpace(s->session, &info, &space);
                }
                s->pose_spaces.push_back(space);
                s->poses.push_back({});
            }
    }

    // ---- one swapchain of a layer an eye for color and one for depth, at the size the runtime recommends
    auto view_count = u32(0);
    xrEnumerateViewConfigurationViews(instance, _state->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                      &view_count, nullptr);
    auto config_views = cc::vector<XrViewConfigurationView>();
    for (auto i = u32(0); i < view_count; ++i)
        config_views.push_back({.type = XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(instance, _state->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count,
                                      &view_count, config_views.data());
    if (view_count == 0)
        return cc::error(error_of("the runtime reports no views for a stereo headset"));
    s->view_count = view_count;
    s->width = int(config_views[0].recommendedImageRectWidth);
    s->height = int(config_views[0].recommendedImageRectHeight);
    for (auto i = u32(0); i < view_count; ++i)
        s->views.push_back({.type = XR_TYPE_VIEW});

    auto color_formats = cc::vector<i64>();
    auto depth_formats = cc::vector<i64>();
#if SXR_HAS_VULKAN
    if (_state->api == graphics_api::vulkan)
    {
        color_formats = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
        depth_formats = {VK_FORMAT_D32_SFLOAT};
    }
#endif
#if SXR_HAS_DX12
    if (_state->api == graphics_api::dx12)
    {
        color_formats = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
        depth_formats = {DXGI_FORMAT_D32_FLOAT};
    }
#endif
    s->color.format = pick_format(s->session, color_formats);
    s->depth.format = pick_format(s->session, depth_formats);
    if (s->color.format == 0 || s->depth.format == 0)
        return cc::error(error_of("the runtime offers no sRGB 8-bit color or 32-bit float depth swapchain format"));

    auto const is_dx12 = _state->api == graphics_api::dx12;
    auto const make_swapchain = [&](impl::swapchain& chain, bool is_depth) -> cc::string
    {
        auto usage = XrSwapchainUsageFlags(is_depth ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                    : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
        if (is_dx12)
            usage |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT; // filled by a copy, see submit_frame
        // dx12 renders into an image of its own, which can always be read
        auto const is_readable = desc.readable_color && !is_depth && !is_dx12;
        if (is_readable)
            usage |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        auto const info = XrSwapchainCreateInfo{.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                                .usageFlags = usage,
                                                .format = chain.format,
                                                .sampleCount = 1,
                                                .width = u32(s->width),
                                                .height = u32(s->height),
                                                .faceCount = 1,
                                                .arraySize = view_count,
                                                .mipCount = 1};
        if (auto const why
            = impl::failed(instance, xrCreateSwapchain(s->session, &info, &chain.handle), "xrCreateSwapchain");
            !why.empty())
            return why;
        auto image_count = u32(0);
        xrEnumerateSwapchainImages(chain.handle, 0, &image_count, nullptr);
        auto const desc = sg::texture_description{
            .format = pixel_format_of(_state->api, chain.format),
            .dimension = sg::texture_dimension::d2,
            .width = s->width,
            .height = s->height,
            .array_layers = int(view_count),
            .usage = is_depth    ? sg::texture_usages(sg::texture_usage::depth_stencil)
                   : is_readable ? sg::texture_usage::render_target | sg::texture_usage::copy_src
                                 : sg::texture_usages(sg::texture_usage::render_target),
        };
#if SXR_HAS_VULKAN
        if (!is_dx12)
        {
            auto images = cc::vector<XrSwapchainImageVulkan2KHR>();
            for (auto i = u32(0); i < image_count; ++i)
                images.push_back({.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
            xrEnumerateSwapchainImages(chain.handle, image_count, &image_count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
            auto& vk = dynamic_cast<sg::backend::vulkan::vulkan_context&>(*s->ctx);
            // the runtime hands every image over in the attachment layout, and takes it back in it
            for (auto const& image : images)
            {
                auto wrapped = vk.wrap_vulkan_texture(
                    image.image, desc,
                    is_depth ? sg::texture_layout::depth_readwrite : sg::texture_layout::render_target);
                if (wrapped.has_error())
                    return cc::format("wrapping a swapchain image failed: {}", wrapped.error().to_string());
                chain.wrapped.push_back(sg::texture_2d_array::from_raw(wrapped.value()));
            }
        }
#endif
#if SXR_HAS_DX12
        if (is_dx12)
        {
            auto images = cc::vector<XrSwapchainImageD3D12KHR>();
            for (auto i = u32(0); i < image_count; ++i)
                images.push_back({.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            xrEnumerateSwapchainImages(chain.handle, image_count, &image_count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
            for (auto const& image : images)
                chain.dx12_images.push_back(image.texture);
        }
#endif
        return {};
    };
    if (auto const why = make_swapchain(s->color, false); !why.empty())
        return cc::error(error_of(why));
    if (auto const why = make_swapchain(s->depth, true); !why.empty())
        return cc::error(error_of(why));

    // dx12's images rest in legacy states sg's enhanced barriers never meet, so sg renders into its own and copies
    if (is_dx12)
    {
        s->own_color = ctx.persistent.create_texture_2d_array(
            {.format = pixel_format_of(_state->api, s->color.format),
             .width = s->width,
             .height = s->height,
             .array_layers = int(view_count),
             .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
        s->own_depth = ctx.persistent.create_texture_2d_array(
            {.format = sg::pixel_format::depth32_float,
             .width = s->width,
             .height = s->height,
             .array_layers = int(view_count),
             .usage = sg::texture_usage::depth_stencil | sg::texture_usage::copy_src});
    }

    // ---- the room behind the frame, running from the start
    auto const blend = impl::choose_blend(desc.blend, _state->runtime_blends, offers(capability::passthrough));
    s->blend = blend.shown;
    if (blend.uses_camera_layer)
    {
        auto const create_passthrough = impl::proc<PFN_xrCreatePassthroughFB>(instance, "xrCreatePassthroughFB");
        auto const create_layer = impl::proc<PFN_xrCreatePassthroughLayerFB>(instance, "xrCreatePassthroughLayerFB");
        auto const passthrough_info = XrPassthroughCreateInfoFB{.type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB,
                                                                .flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB};
        auto const layer_info = [&]
        {
            return XrPassthroughLayerCreateInfoFB{.type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB,
                                                  .passthrough = s->passthrough,
                                                  .flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB,
                                                  .purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB};
        };
        auto why = impl::failed(instance, create_passthrough(s->session, &passthrough_info, &s->passthrough),
                                "xrCreatePassthroughFB");
        if (why.empty())
        {
            auto const info = layer_info();
            why = impl::failed(instance, create_layer(s->session, &info, &s->passthrough_layer),
                               "xrCreatePassthroughLayerFB");
        }
        if (!why.empty())
        {
            CC_LOG_WARNING("{}, so the frame shows no room behind it", why);
            s->blend = blend_mode::opaque;
        }
    }
    else if (s->blend != desc.blend)
        CC_LOG_WARNING("a blend mode was asked of a session whose system did not want passthrough, or whose runtime "
                       "has neither");

    CC_LOG_INFO("{} views of {} x {}", view_count, s->width, s->height);
    return cc::make_unique<session>(cc::move(s));
}

bool sxr::session::poll_events()
{
    auto& s = *_state;
    auto event = XrEventDataBuffer{.type = XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(s.sys->instance, &event) == XR_SUCCESS)
    {
        if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
            return false;
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
        {
            s.current = reinterpret_cast<XrEventDataSessionStateChanged const&>(event).state;
            CC_LOG_DEBUG("session state {}", int(s.current));
            if (s.current == XR_SESSION_STATE_READY)
            {
                auto const begin
                    = XrSessionBeginInfo{.type = XR_TYPE_SESSION_BEGIN_INFO,
                                         .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                s.is_running = XR_SUCCEEDED(xrBeginSession(s.session, &begin));
            }
            else if (s.current == XR_SESSION_STATE_STOPPING)
            {
                xrEndSession(s.session);
                s.is_running = false;
            }
            else if (s.current == XR_SESSION_STATE_EXITING || s.current == XR_SESSION_STATE_LOSS_PENDING)
                return false;
        }
        event = XrEventDataBuffer{.type = XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

bool sxr::session::is_running() const
{
    return _state->is_running;
}
bool sxr::session::is_focused() const
{
    return _state->current == XR_SESSION_STATE_FOCUSED;
}
sxr::availability sxr::session::availability_of(capability c) const
{
    // TEMPORARY(feat-android-apk): nothing asks a user yet, so needs_permission and denied wait for Android's permissions
    for (auto const offered : _state->sys->offered)
        if (offered == c)
            return availability::available;
    return availability::unsupported;
}

sxr::blend_mode sxr::session::blend() const
{
    return _state->blend;
}

cc::optional<sxr::frame> sxr::session::wait_frame()
{
    auto& s = *_state;
    CC_ASSERT(!s.in_frame, "wait_frame: the previous frame was never submitted");
    if (!s.is_running)
        return {};

    auto frame_state = XrFrameState{.type = XR_TYPE_FRAME_STATE};
    auto const wait_info = XrFrameWaitInfo{.type = XR_TYPE_FRAME_WAIT_INFO};
    if (XR_FAILED(xrWaitFrame(s.session, &wait_info, &frame_state)))
        return {};
    auto const begin_info = XrFrameBeginInfo{.type = XR_TYPE_FRAME_BEGIN_INFO};
    xrBeginFrame(s.session, &begin_info);
    s.in_frame = true;
    s.display_time = frame_state.predictedDisplayTime;

    auto view_state = XrViewState{.type = XR_TYPE_VIEW_STATE};
    auto const locate = XrViewLocateInfo{.type = XR_TYPE_VIEW_LOCATE_INFO,
                                         .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                         .displayTime = s.display_time,
                                         .space = s.space};
    auto located = u32(0);
    for (auto& v : s.views)
        v = {.type = XR_TYPE_VIEW};
    xrLocateViews(s.session, &locate, &view_state, s.view_count, &located, s.views.data());
    auto const tracked = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    s.frame_renders
        = frame_state.shouldRender && located == s.view_count && (view_state.viewStateFlags & tracked) == tracked;

    // the actions, synced while focused and released otherwise, and the poses where they are at display time
    if (s.action_set != XR_NULL_HANDLE)
    {
        if (s.current == XR_SESSION_STATE_FOCUSED)
        {
            auto const active = XrActiveActionSet{.actionSet = s.action_set, .subactionPath = XR_NULL_PATH};
            auto const sync = XrActionsSyncInfo{.type = XR_TYPE_ACTIONS_SYNC_INFO,
                                                .countActiveActionSets = 1,
                                                .activeActionSets = &active};
            xrSyncActions(s.session, &sync);
        }
        for (auto i = isize(0); i < s.pose_spaces.size(); ++i)
        {
            s.poses[i] = {};
            if (s.pose_spaces[i] == XR_NULL_HANDLE)
                continue;
            auto location = XrSpaceLocation{.type = XR_TYPE_SPACE_LOCATION};
            auto const valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if (XR_SUCCEEDED(xrLocateSpace(s.pose_spaces[i], s.space, s.display_time, &location))
                && (location.locationFlags & valid) == valid)
                s.poses[i] = impl::camera_to_world_of({.qx = location.pose.orientation.x,
                                                       .qy = location.pose.orientation.y,
                                                       .qz = location.pose.orientation.z,
                                                       .qw = location.pose.orientation.w,
                                                       .px = location.pose.position.x,
                                                       .py = location.pose.position.y,
                                                       .pz = location.pose.position.z});
        }
    }

    auto f = frame{.should_render = s.frame_renders, .width = s.width, .height = s.height};
    if (!s.frame_renders)
        return f;

    for (auto const& v : s.views)
        f.views.push_back(
            {.camera_to_world = impl::camera_to_world_of({.qx = v.pose.orientation.x,
                                                          .qy = v.pose.orientation.y,
                                                          .qz = v.pose.orientation.z,
                                                          .qw = v.pose.orientation.w,
                                                          .px = v.pose.position.x,
                                                          .py = v.pose.position.y,
                                                          .pz = v.pose.position.z}),
             .projection = impl::projection_of(
                 {.left = v.fov.angleLeft, .right = v.fov.angleRight, .up = v.fov.angleUp, .down = v.fov.angleDown},
                 s.desc.near_distance, s.desc.far_distance)});

    for (auto* chain : {&s.color, &s.depth})
    {
        auto const acquire_info = XrSwapchainImageAcquireInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        xrAcquireSwapchainImage(chain->handle, &acquire_info, &chain->acquired);
        auto const wait_image
            = XrSwapchainImageWaitInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
        xrWaitSwapchainImage(chain->handle, &wait_image);
    }
    f.color = s.color.wrapped.empty() ? s.own_color : s.color.wrapped[s.color.acquired];
    f.depth = s.depth.wrapped.empty() ? s.own_depth : s.depth.wrapped[s.depth.acquired];
    return f;
}

isize sxr::session::state::action_index(cc::string_view name) const
{
    for (auto i = isize(0); i < desc.actions.size(); ++i)
        if (desc.actions[i].name == name)
            return i;
    CC_ASSERTF(false, "the session was created with no action named {}", name);
    return -1;
}

namespace
{
/// What an action is read for: its handle and, for one per hand, which.
[[nodiscard]] XrActionStateGetInfo state_info(sxr::session::state const& s, isize i, sxr::hand h)
{
    return {.type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = s.actions[i],
            .subactionPath = s.desc.actions[i].is_per_hand ? s.hand_paths[int(h)] : XR_NULL_PATH};
}
} // namespace

bool sxr::session::pressed(cc::string_view action, hand h) const
{
    auto const& s = *_state;
    auto const info = state_info(s, s.action_index(action), h);
    auto state = XrActionStateBoolean{.type = XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(s.session, &info, &state)) && state.isActive && state.currentState;
}

sxr::f32 sxr::session::scalar(cc::string_view action, hand h) const
{
    auto const& s = *_state;
    auto const info = state_info(s, s.action_index(action), h);
    auto state = XrActionStateFloat{.type = XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(s.session, &info, &state)) || !state.isActive)
        return 0.0f;
    return state.currentState;
}

tg::vec2f sxr::session::vector2(cc::string_view action, hand h) const
{
    auto const& s = *_state;
    auto const info = state_info(s, s.action_index(action), h);
    auto state = XrActionStateVector2f{.type = XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_FAILED(xrGetActionStateVector2f(s.session, &info, &state)) || !state.isActive)
        return {};
    return tg::vec2f(state.currentState.x, state.currentState.y);
}

cc::optional<tg::rigid_transform3f> sxr::session::pose(cc::string_view action, hand h) const
{
    auto const& s = *_state;
    auto const i = s.action_index(action);
    return s.poses[i * 2 + (s.desc.actions[i].is_per_hand ? int(h) : 0)];
}

void sxr::session::vibrate(cc::string_view action, hand h, f32 amplitude, f32 seconds)
{
    auto const& s = *_state;
    auto const i = s.action_index(action);
    auto const info
        = XrHapticActionInfo{.type = XR_TYPE_HAPTIC_ACTION_INFO,
                             .action = s.actions[i],
                             .subactionPath = s.desc.actions[i].is_per_hand ? s.hand_paths[int(h)] : XR_NULL_PATH};
    auto const vibration = XrHapticVibration{.type = XR_TYPE_HAPTIC_VIBRATION,
                                             .duration = XrDuration(f64(seconds) * 1e9),
                                             .frequency = XR_FREQUENCY_UNSPECIFIED,
                                             .amplitude = amplitude};
    xrApplyHapticFeedback(s.session, &info, reinterpret_cast<XrHapticBaseHeader const*>(&vibration));
}

void sxr::session::submit_frame(std::unique_ptr<sg::command_list> cmd)
{
    auto& s = *_state;
    CC_ASSERT(s.in_frame, "submit_frame: no frame was waited for");
    s.in_frame = false;

    if (s.frame_renders)
    {
        CC_ASSERT(cmd != nullptr, "submit_frame: a frame that should render needs the list that drew it");
#if SXR_HAS_VULKAN
        // the runtime takes each image back in the layout it handed it over in
        if (!s.color.wrapped.empty())
        {
            cmd->ensure_layout(s.color.wrapped[s.color.acquired].raw(), sg::texture_layout::render_target);
            cmd->ensure_layout(s.depth.wrapped[s.depth.acquired].raw(), sg::texture_layout::depth_readwrite);
        }
#endif
#if SXR_HAS_DX12
        if (s.color.wrapped.empty())
        {
            namespace dx12 = sg::backend::dx12;
            dx12::native_texture_access const reads[] = {
                {.texture = s.own_color.raw(),
                 .access = sg::access_flag::copy_read,
                 .stages = sg::pipeline_stage_flag::copy},
                {.texture = s.own_depth.raw(),
                 .access = sg::access_flag::copy_read,
                 .stages = sg::pipeline_stage_flag::copy},
            };
            auto const scope = dx12::dx12_native_scope::open(*cmd, reads);
            auto const copy = [&](ID3D12Resource* image, ID3D12Resource* source, D3D12_RESOURCE_STATES resting)
            {
                auto barrier = D3D12_RESOURCE_BARRIER{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
                barrier.Transition = {.pResource = image,
                                      .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                      .StateBefore = resting,
                                      .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST};
                scope.list()->ResourceBarrier(1, &barrier);
                scope.list()->CopyResource(image, source);
                cc::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                scope.list()->ResourceBarrier(1, &barrier);
            };
            // the runtime hands color over in RENDER_TARGET and depth in DEPTH_WRITE, and takes each back so
            copy(s.color.dx12_images[s.color.acquired], scope.resource(s.own_color.raw()),
                 D3D12_RESOURCE_STATE_RENDER_TARGET);
            copy(s.depth.dx12_images[s.depth.acquired], scope.resource(s.own_depth.raw()),
                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
#endif
        // submitted before the images are released, so the runtime's work on the same queue comes after it
        s.ctx->submit_command_list(cc::move(cmd));
        for (auto* chain : {&s.color, &s.depth})
        {
            auto const release_info = XrSwapchainImageReleaseInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(chain->handle, &release_info);
        }
    }
    else if (cmd != nullptr)
        s.ctx->submit_command_list(cc::move(cmd));

    // ---- the layers: the room underneath, the frame over it by its alpha, each view with its depth
    auto depth_infos = cc::vector<XrCompositionLayerDepthInfoKHR>();
    auto projection_views = cc::vector<XrCompositionLayerProjectionView>();
    if (s.frame_renders)
    {
        depth_infos.reserve(s.view_count);
        for (auto i = u32(0); i < s.view_count; ++i)
        {
            depth_infos.push_back({.type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR,
                                   .subImage = {.swapchain = s.depth.handle,
                                                .imageRect = {.offset = {0, 0}, .extent = {s.width, s.height}},
                                                .imageArrayIndex = i},
                                   .minDepth = 0.0f,
                                   .maxDepth = 1.0f,
                                   .nearZ = s.desc.near_distance,
                                   .farZ = s.desc.far_distance});
            projection_views.push_back({.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
                                        .next = s.sys->offers_depth_layer ? &depth_infos.back() : nullptr,
                                        .pose = s.views[i].pose,
                                        .fov = s.views[i].fov,
                                        .subImage = {.swapchain = s.color.handle,
                                                     .imageRect = {.offset = {0, 0}, .extent = {s.width, s.height}},
                                                     .imageArrayIndex = i}});
        }
    }
    auto const shows_room = s.passthrough_layer != XR_NULL_HANDLE;
    auto const room = XrCompositionLayerPassthroughFB{.type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB,
                                                      .flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                                                      .space = XR_NULL_HANDLE,
                                                      .layerHandle = s.passthrough_layer};
    auto const projection = XrCompositionLayerProjection{
        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
        .layerFlags = s.blend == blend_mode::alpha_blend
                        ? XrCompositionLayerFlags(XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT)
                        : 0,
        .space = s.space,
        .viewCount = u32(projection_views.size()),
        .views = projection_views.data()};
    // with a camera layer the runtime has no blending of its own to do, whatever the frame's mode
    auto environment = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (!shows_room && s.blend == blend_mode::alpha_blend)
        environment = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    if (s.blend == blend_mode::additive)
        environment = XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    auto layers = cc::vector<XrCompositionLayerBaseHeader const*>();
    if (shows_room)
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader const*>(&room));
    if (s.frame_renders)
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader const*>(&projection));
    auto const end_info = XrFrameEndInfo{.type = XR_TYPE_FRAME_END_INFO,
                                         .displayTime = s.display_time,
                                         .environmentBlendMode = environment,
                                         .layerCount = u32(layers.size()),
                                         .layers = layers.empty() ? nullptr : layers.data()};
    if (auto const why = impl::failed(s.sys->instance, xrEndFrame(s.session, &end_info), "xrEndFrame"); !why.empty())
        CC_LOG_WARNING("{}", why);
}

#endif
