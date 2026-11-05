#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/all.hh>
#include <shaped-xr/impl/openxr.hh>
#include <shaped-xr/impl/view_math.hh>

#if XR_HAS_DX12
#include <shaped-graphics/backends/dx12/dx12_native_scope.hh>

using namespace cc::primitive_defines;
#endif

xr::session::session(cc::unique_ptr<state> s) : _state(cc::move(s))
{
}
xr::session::~session() = default;

#if !XR_HAS_OPENXR

struct xr::session::state
{
};
bool xr::session::poll_events()
{
    return false;
}
bool xr::session::is_running() const
{
    return false;
}
bool xr::session::is_focused() const
{
    return false;
}
bool xr::session::shows_passthrough() const
{
    return false;
}
cc::optional<xr::frame> xr::session::wait_frame()
{
    return {};
}
void xr::session::submit_frame(std::unique_ptr<sg::command_list>)
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
[[nodiscard]] sg::pixel_format pixel_format_of(xr::graphics_api api, i64 format)
{
#if XR_HAS_VULKAN
    if (api == xr::graphics_api::vulkan)
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
#if XR_HAS_DX12
    if (api == xr::graphics_api::dx12)
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

xr::session::state::~state()
{
    auto const destroy_layer
        = impl::proc<PFN_xrDestroyPassthroughLayerFB>(sys->instance, "xrDestroyPassthroughLayerFB");
    auto const destroy_passthrough = impl::proc<PFN_xrDestroyPassthroughFB>(sys->instance, "xrDestroyPassthroughFB");
    if (passthrough_layer != XR_NULL_HANDLE && destroy_layer != nullptr)
        destroy_layer(passthrough_layer);
    if (passthrough != XR_NULL_HANDLE && destroy_passthrough != nullptr)
        destroy_passthrough(passthrough);
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

cc::result<cc::unique_ptr<xr::session>> xr::system::create_session(sg::context& ctx, session_description const& desc)
{
    auto s = cc::make_unique<session::state>();
    s->sys = _state.get();
    s->ctx = &ctx;
    s->desc = desc;
    auto const instance = _state->instance;

    // ---- the session over the context's device and queue
#if XR_HAS_VULKAN
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
#if XR_HAS_DX12
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
#if XR_HAS_VULKAN
    if (_state->api == graphics_api::vulkan)
        binding = &vk_binding;
#endif
#if XR_HAS_DX12
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
#if XR_HAS_VULKAN
    if (_state->api == graphics_api::vulkan)
    {
        color_formats = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
        depth_formats = {VK_FORMAT_D32_SFLOAT};
    }
#endif
#if XR_HAS_DX12
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
            .usage = is_depth ? sg::texture_usages(sg::texture_usage::depth_stencil)
                              : sg::texture_usages(sg::texture_usage::render_target),
        };
#if XR_HAS_VULKAN
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
#if XR_HAS_DX12
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
    if (desc.passthrough && _state->offers_passthrough)
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
            CC_LOG_WARNING("{}, so the frame shows no room behind it", why);
    }
    else if (desc.passthrough)
        CC_LOG_WARNING("passthrough was asked of a session whose system did not ask for it, or whose runtime has none");

    CC_LOG_INFO("{} views of {} x {}", view_count, s->width, s->height);
    return cc::make_unique<session>(cc::move(s));
}

bool xr::session::poll_events()
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

bool xr::session::is_running() const
{
    return _state->is_running;
}
bool xr::session::is_focused() const
{
    return _state->current == XR_SESSION_STATE_FOCUSED;
}
bool xr::session::shows_passthrough() const
{
    return _state->passthrough_layer != XR_NULL_HANDLE;
}

cc::optional<xr::frame> xr::session::wait_frame()
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

void xr::session::submit_frame(std::unique_ptr<sg::command_list> cmd)
{
    auto& s = *_state;
    CC_ASSERT(s.in_frame, "submit_frame: no frame was waited for");
    s.in_frame = false;

    if (s.frame_renders)
    {
        CC_ASSERT(cmd != nullptr, "submit_frame: a frame that should render needs the list that drew it");
#if XR_HAS_VULKAN
        // the runtime takes each image back in the layout it handed it over in
        if (!s.color.wrapped.empty())
        {
            cmd->ensure_layout(s.color.wrapped[s.color.acquired].raw(), sg::texture_layout::render_target);
            cmd->ensure_layout(s.depth.wrapped[s.depth.acquired].raw(), sg::texture_layout::depth_readwrite);
        }
#endif
#if XR_HAS_DX12
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
        .layerFlags = shows_room ? XrCompositionLayerFlags(XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) : 0,
        .space = s.space,
        .viewCount = u32(projection_views.size()),
        .views = projection_views.data()};
    auto layers = cc::vector<XrCompositionLayerBaseHeader const*>();
    if (shows_room)
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader const*>(&room));
    if (s.frame_renders)
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader const*>(&projection));
    auto const end_info = XrFrameEndInfo{.type = XR_TYPE_FRAME_END_INFO,
                                         .displayTime = s.display_time,
                                         .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                                         .layerCount = u32(layers.size()),
                                         .layers = layers.empty() ? nullptr : layers.data()};
    if (auto const why = impl::failed(s.sys->instance, xrEndFrame(s.session, &end_info), "xrEndFrame"); !why.empty())
        CC_LOG_WARNING("{}", why);
}

#endif
