// The SGL cube in a headset, over Quest Link or any other OpenXR runtime on Windows, on dx12.
//
// A prototype: it talks to OpenXR directly and draws the eyes one at a time, shortcuts each marked against the issue
// that replaces it.
// Each eye renders into an sg texture a native copy moves into the runtime's image, which is how dx12 fills one.

#include <clean-core/common/time.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_native_scope.hh>
#include <shaped-shader-library/compiler/available_compilers.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/camera/camera.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/transform/compose.hh>
#include <xr_cube_shaders.hh>

#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

using namespace cc::primitive_defines;

namespace
{
using cube_vertex = shaders::cube::cube_vertex;

constexpr int cube_index_count = 36;
constexpr auto eye_format = sg::pixel_format::rgba8_unorm_srgb;
constexpr auto eye_dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

// sgl-cube's mesh: four corners a face, so a shared corner carries three different normals
[[nodiscard]] cc::array<cube_vertex> build_cube_mesh()
{
    tg::vec3f const normals[]
        = {tg::vec3f(0, 0, -1), tg::vec3f(0, 0, 1), tg::vec3f(-1, 0, 0), tg::vec3f(1, 0, 0), tg::vec3f(0, -1, 0), tg::vec3f(0, 1, 0)};
    tg::vec3f const colors[] = {tg::vec3f(0.90f, 0.32f, 0.30f), tg::vec3f(0.30f, 0.62f, 0.90f), tg::vec3f(0.42f, 0.80f, 0.42f),
                                tg::vec3f(0.94f, 0.74f, 0.28f), tg::vec3f(0.66f, 0.44f, 0.88f), tg::vec3f(0.94f, 0.94f, 0.92f)};
    auto out = cc::array<cube_vertex>::create_defaulted(24);
    for (auto face = 0; face < 6; ++face)
    {
        auto const n = normals[face];
        auto const u = tg::vec3f(n[1], n[2], n[0]);
        auto const v = tg::dual(tg::cross(n, u));
        for (auto corner = 0; corner < 4; ++corner)
        {
            auto const su = (corner == 1 || corner == 2) ? 1.0f : -1.0f;
            auto const sv = (corner >= 2) ? 1.0f : -1.0f;
            out[face * 4 + corner]
                = {.position = tg::pos3f::zero + (n + u * su + v * sv) * 0.5f, .normal = n, .color = colors[face]};
        }
    }
    return out;
}

[[nodiscard]] cc::array<u16> build_cube_indices()
{
    auto out = cc::array<u16>::create_defaulted(cube_index_count);
    for (auto face = 0; face < 6; ++face)
    {
        u16 const quad[] = {0, 2, 1, 0, 3, 2};
        for (auto i = 0; i < 6; ++i)
            out[face * 6 + i] = u16(face * 4 + quad[i]);
    }
    return out;
}

/// `result` as a message naming `what`, or empty where it succeeded.
[[nodiscard]] cc::string failed(XrInstance instance, XrResult result, cc::string_view what)
{
    if (XR_SUCCEEDED(result))
        return {};
    char name[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance != XR_NULL_HANDLE)
        xrResultToString(instance, result, name);
    return cc::format("{} failed: {} ({})", what, name[0] != 0 ? name : "unknown", int(result));
}

/// An eye's camera-to-world pose in tg's left-handed frame: OpenXR's is right-handed with -z forward, so z flips.
[[nodiscard]] tg::mat4f camera_to_world(XrPosef const& pose)
{
    auto const x = -pose.orientation.x;
    auto const y = -pose.orientation.y;
    auto const z = pose.orientation.z;
    auto const w = pose.orientation.w;
    auto m = tg::mat4f::identity;
    m[0, 0] = 1 - 2 * (y * y + z * z);
    m[0, 1] = 2 * (x * y + z * w);
    m[0, 2] = 2 * (x * z - y * w);
    m[1, 0] = 2 * (x * y - z * w);
    m[1, 1] = 1 - 2 * (x * x + z * z);
    m[1, 2] = 2 * (y * z + x * w);
    m[2, 0] = 2 * (x * z + y * w);
    m[2, 1] = 2 * (y * z - x * w);
    m[2, 2] = 1 - 2 * (x * x + y * y);
    m[3, 0] = pose.position.x;
    m[3, 1] = pose.position.y;
    m[3, 2] = -pose.position.z;
    return m;
}

/// The cube's world matrix: a 30 cm cube a metre ahead of where the session started, at eye height, spinning.
[[nodiscard]] tg::mat4f cube_to_world(tg::angle_f spin)
{
    auto const rotation = tg::mat3f::make_rotation_y(spin) * 0.3f;
    auto m = tg::mat4f::identity;
    for (auto c = 0; c < 3; ++c)
        for (auto r = 0; r < 3; ++r)
            m[c, r] = rotation[c, r];
    m[3, 2] = 1.0f;
    return m;
}

/// One eye's swapchain and the sg texture it is rendered into before the copy.
struct eye
{
    XrSwapchain swapchain = XR_NULL_HANDLE;
    cc::vector<XrSwapchainImageD3D12KHR> images;
    int width = 0;
    int height = 0;
    // the runtime's image rests in legacy RENDER_TARGET, which sg's enhanced barriers never meet, so sg renders here
    sg::texture_2d target;
};

/// Everything OpenXR handed out, torn down in reverse.
struct xr_state
{
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    cc::vector<eye> eyes;
    // the headset's cameras behind the cube, where the runtime offers XR_FB_passthrough and its user enabled it
    bool offers_passthrough = false;
    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthrough_layer = XR_NULL_HANDLE;
    PFN_xrDestroyPassthroughFB destroy_passthrough = nullptr;
    PFN_xrDestroyPassthroughLayerFB destroy_passthrough_layer = nullptr;

    ~xr_state()
    {
        if (passthrough_layer != XR_NULL_HANDLE)
            destroy_passthrough_layer(passthrough_layer);
        if (passthrough != XR_NULL_HANDLE)
            destroy_passthrough(passthrough);
        for (auto& e : eyes)
            if (e.swapchain != XR_NULL_HANDLE)
                xrDestroySwapchain(e.swapchain);
        if (space != XR_NULL_HANDLE)
            xrDestroySpace(space);
        if (session != XR_NULL_HANDLE)
            xrDestroySession(session);
        if (instance != XR_NULL_HANDLE)
            xrDestroyInstance(instance);
    }
};

[[nodiscard]] cc::string create_instance(xr_state& xr)
{
    auto available_count = u32(0);
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &available_count, nullptr);
    auto available = cc::vector<XrExtensionProperties>();
    for (auto i = u32(0); i < available_count; ++i)
        available.push_back({.type = XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, available_count, &available_count, available.data());
    for (auto const& e : available)
        if (cc::string_view(e.extensionName) == XR_FB_PASSTHROUGH_EXTENSION_NAME)
            xr.offers_passthrough = true;

    char const* const extensions[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME, XR_FB_PASSTHROUGH_EXTENSION_NAME};
    auto info = XrInstanceCreateInfo{.type = XR_TYPE_INSTANCE_CREATE_INFO,
                                     .enabledExtensionCount = xr.offers_passthrough ? 2u : 1u,
                                     .enabledExtensionNames = extensions};
    std::snprintf(info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "shaped-core xr-cube");
    std::snprintf(info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "shaped-core");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    if (auto const why = failed(XR_NULL_HANDLE, xrCreateInstance(&info, &xr.instance), "xrCreateInstance"); !why.empty())
        return cc::format("{}: no OpenXR runtime is active, or it has no D3D12 binding", why);

    auto const system_info = XrSystemGetInfo{.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    if (auto const why = failed(xr.instance, xrGetSystem(xr.instance, &system_info, &xr.system), "xrGetSystem"); !why.empty())
        return cc::format("{}: no headset is connected (is Quest Link running in the headset?)", why);
    return {};
}
} // namespace

ASYNC_EXAMPLE("shaped-graphics/xr-cube")
{
    // TEMPORARY(feat-shaped-xr-core): OpenXR is called directly, with no library around it
    auto xr = xr_state();
    if (auto const why = create_instance(xr); !why.empty())
    {
        cc::eprintln("{}", why);
        FAIL("xr-cube cannot run, as printed above");
        co_return;
    }
    auto properties = XrSystemProperties{.type = XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(xr.instance, xr.system, &properties);
    cc::println("xr: {} on {}", properties.systemName, [&]
    {
        auto runtime = XrInstanceProperties{.type = XR_TYPE_INSTANCE_PROPERTIES};
        xrGetInstanceProperties(xr.instance, &runtime);
        return cc::string(runtime.runtimeName);
    }());

    // the runtime names the adapter it composes on, and the context must be created on it
    auto get_requirements = PFN_xrGetD3D12GraphicsRequirementsKHR(nullptr);
    xrGetInstanceProcAddr(xr.instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&get_requirements);
    auto requirements = XrGraphicsRequirementsD3D12KHR{.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    if (auto const why = failed(xr.instance, get_requirements(xr.instance, xr.system, &requirements), "xrGetD3D12GraphicsRequirementsKHR");
        !why.empty())
    {
        cc::eprintln("{}", why);
        FAIL("xr-cube cannot run, as printed above");
        co_return;
    }
    auto const luid = u64(requirements.adapterLuid.LowPart) | (u64(u32(requirements.adapterLuid.HighPart)) << 32);
    auto const created = sg::create_dx12_context({.adapter_luid = luid, .minimum_feature_level = requirements.minFeatureLevel});
    if (created.has_error())
    {
        cc::eprintln("no dx12 device on the runtime's adapter: {}", created.error().to_string());
        FAIL("xr-cube cannot run, as printed above");
        co_return;
    }
    auto const ctx = created.value();
    auto* const dx = dynamic_cast<sg::backend::dx12::dx12_context*>(ctx.get());

    auto binding = XrGraphicsBindingD3D12KHR{.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR, .device = dx->device(), .queue = dx->queue()};
    auto const session_info = XrSessionCreateInfo{.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = xr.system};
    if (auto const why = failed(xr.instance, xrCreateSession(xr.instance, &session_info, &xr.session), "xrCreateSession"); !why.empty())
    {
        cc::eprintln("{}", why);
        FAIL("xr-cube cannot run, as printed above");
        co_return;
    }
    auto const space_info = XrReferenceSpaceCreateInfo{.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
                                                       .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                       .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}}};
    xrCreateReferenceSpace(xr.session, &space_info, &xr.space);

    // TEMPORARY(feat-xr-passthrough): the runtime's own passthrough, called directly as everything else here is
    if (xr.offers_passthrough)
    {
        auto create_passthrough = PFN_xrCreatePassthroughFB(nullptr);
        auto create_layer = PFN_xrCreatePassthroughLayerFB(nullptr);
        xrGetInstanceProcAddr(xr.instance, "xrCreatePassthroughFB", (PFN_xrVoidFunction*)&create_passthrough);
        xrGetInstanceProcAddr(xr.instance, "xrCreatePassthroughLayerFB", (PFN_xrVoidFunction*)&create_layer);
        xrGetInstanceProcAddr(xr.instance, "xrDestroyPassthroughFB", (PFN_xrVoidFunction*)&xr.destroy_passthrough);
        xrGetInstanceProcAddr(xr.instance, "xrDestroyPassthroughLayerFB", (PFN_xrVoidFunction*)&xr.destroy_passthrough_layer);
        auto const passthrough_info
            = XrPassthroughCreateInfoFB{.type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB, .flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB};
        auto const created_passthrough = create_passthrough(xr.session, &passthrough_info, &xr.passthrough);
        if (XR_SUCCEEDED(created_passthrough))
        {
            auto const layer_info = XrPassthroughLayerCreateInfoFB{.type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB,
                                                                   .passthrough = xr.passthrough,
                                                                   .flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB,
                                                                   .purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB};
            if (auto const why = failed(xr.instance, create_layer(xr.session, &layer_info, &xr.passthrough_layer), "xrCreatePassthroughLayerFB");
                !why.empty())
                cc::println("xr: {}, so the cube stands in the dark", why);
        }
        else
            cc::println("xr: {} — over Link, passthrough needs the Link app's Settings > Beta > Developer Runtime Features "
                        "and Passthrough over Meta Quest Link",
                        failed(xr.instance, created_passthrough, "xrCreatePassthroughFB"));
    }
    else
        cc::println("xr: the runtime offers no XR_FB_passthrough, so the cube stands in the dark");
    auto const sees_through = xr.passthrough_layer != XR_NULL_HANDLE;
    if (sees_through)
        cc::println("xr: passthrough on");

    // one swapchain an eye, at the size the runtime recommends
    auto view_count = u32(0);
    xrEnumerateViewConfigurationViews(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    auto config_views = cc::vector<XrViewConfigurationView>();
    for (auto i = u32(0); i < view_count; ++i)
        config_views.push_back({.type = XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count,
                                      config_views.data());
    for (auto const& v : config_views)
    {
        auto e = eye{.width = int(v.recommendedImageRectWidth), .height = int(v.recommendedImageRectHeight)};
        auto const info = XrSwapchainCreateInfo{.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                                .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                                                .format = eye_dxgi_format,
                                                .sampleCount = 1,
                                                .width = v.recommendedImageRectWidth,
                                                .height = v.recommendedImageRectHeight,
                                                .faceCount = 1,
                                                .arraySize = 1,
                                                .mipCount = 1};
        if (auto const why = failed(xr.instance, xrCreateSwapchain(xr.session, &info, &e.swapchain), "xrCreateSwapchain"); !why.empty())
        {
            cc::eprintln("{}", why);
            FAIL("xr-cube cannot run, as printed above");
            co_return;
        }
        auto image_count = u32(0);
        xrEnumerateSwapchainImages(e.swapchain, 0, &image_count, nullptr);
        for (auto i = u32(0); i < image_count; ++i)
            e.images.push_back({.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        xrEnumerateSwapchainImages(e.swapchain, image_count, &image_count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(e.images.data()));
        e.target = ctx->persistent.create_texture_2d({.format = eye_format,
                                                      .width = e.width,
                                                      .height = e.height,
                                                      .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});
        xr.eyes.push_back(cc::move(e));
    }
    cc::println("xr: {} eyes of {} x {}", xr.eyes.size(), xr.eyes[0].width, xr.eyes[0].height);

    auto lib = slib::shader_library();
    slib::add_available_compilers(lib);
    lib.add_package(shaders::package());
    auto const built = ctx->cached.acquire_raster_pipeline(shaders::cube::pipeline, {.color = eye_format});
    co_await cc::async_settled(built);
    if (built->try_value() == nullptr)
    {
        cc::eprintln("the cube's pipeline did not build");
        FAIL("xr-cube cannot run, as printed above");
        co_return;
    }
    auto const pipeline = *built->try_value();
    auto const vertices = ctx->persistent.create_buffer_from_data(build_cube_mesh(), sg::buffer_usage::vertex_buffer);
    auto const indices = ctx->persistent.create_buffer_from_data(build_cube_indices(), sg::buffer_usage::index_buffer);

    auto state = XR_SESSION_STATE_UNKNOWN;
    auto is_running = false;
    auto frames = u64(0);
    auto const start = cc::current_time_steady_secs();
    while (true)
    {
        // ---- events: the session begins when the runtime is ready, and ends when it stops or the app is closed
        auto quit = false;
        auto event = XrEventDataBuffer{.type = XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(xr.instance, &event) == XR_SUCCESS)
        {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                state = reinterpret_cast<XrEventDataSessionStateChanged const&>(event).state;
                cc::println("xr: session state {}", int(state));
                if (state == XR_SESSION_STATE_READY)
                {
                    auto const begin = XrSessionBeginInfo{.type = XR_TYPE_SESSION_BEGIN_INFO,
                                                          .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                    is_running = XR_SUCCEEDED(xrBeginSession(xr.session, &begin));
                }
                else if (state == XR_SESSION_STATE_STOPPING)
                {
                    xrEndSession(xr.session);
                    is_running = false;
                }
                else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING)
                    quit = true;
            }
            else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
                quit = true;
            event = XrEventDataBuffer{.type = XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (quit)
            break;
        if (!is_running)
        {
            cc::this_thread_sleep_secs(0.05); // nothing to pace frames by until the runtime is ready
            continue;
        }

        // ---- the frame, paced by the runtime
        auto frame_state = XrFrameState{.type = XR_TYPE_FRAME_STATE};
        auto const wait_info = XrFrameWaitInfo{.type = XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(xr.session, &wait_info, &frame_state)))
            break;
        auto const begin_info = XrFrameBeginInfo{.type = XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(xr.session, &begin_info);

        auto views = cc::vector<XrView>();
        for (auto i = isize(0); i < xr.eyes.size(); ++i)
            views.push_back({.type = XR_TYPE_VIEW});
        auto view_state = XrViewState{.type = XR_TYPE_VIEW_STATE};
        auto const locate = XrViewLocateInfo{.type = XR_TYPE_VIEW_LOCATE_INFO,
                                             .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             .displayTime = frame_state.predictedDisplayTime,
                                             .space = xr.space};
        auto located = u32(0);
        xrLocateViews(xr.session, &locate, &view_state, u32(views.size()), &located, views.data());
        auto const is_tracked = (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0
                             && (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;

        auto projection_views = cc::vector<XrCompositionLayerProjectionView>();
        auto const should_render = frame_state.shouldRender && is_tracked && located == u32(xr.eyes.size());
        if (should_render)
        {
            auto const spin = tg::angle_f::make_from_degree(float(cc::current_time_steady_secs() - start) * 30.0f);
            auto cmd = ctx->create_command_list();
            auto acquired = cc::vector<u32>();
            for (auto i = isize(0); i < xr.eyes.size(); ++i)
            {
                auto& e = xr.eyes[i];
                auto const& view = views[i];
                auto index = u32(0);
                auto const acquire_info = XrSwapchainImageAcquireInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                xrAcquireSwapchainImage(e.swapchain, &acquire_info, &index);
                auto const wait_image = XrSwapchainImageWaitInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
                xrWaitSwapchainImage(e.swapchain, &wait_image);
                acquired.push_back(index);

                // the eye's frustum, its four angles as extents on the near plane
                auto const near_distance = 0.05f;
                auto const projection = tg::perspective_off_center_lh_zero_to_one(
                    tg::tan(tg::angle_f::make_from_radians(view.fov.angleLeft)) * near_distance,
                    tg::tan(tg::angle_f::make_from_radians(view.fov.angleRight)) * near_distance,
                    tg::tan(tg::angle_f::make_from_radians(view.fov.angleDown)) * near_distance,
                    tg::tan(tg::angle_f::make_from_radians(view.fov.angleUp)) * near_distance, near_distance, 100.0f);
                auto const transform = projection.to_mat() * camera_to_world(view.pose).inverse() * cube_to_world(spin);

                // one pass an eye; feat-xr-cube-example draws both in one, through view instancing
                {
                    auto const depth = ctx->transient.create_texture_2d(
                        {.format = sg::pixel_format::depth32_float, .width = e.width, .height = e.height, .usage = sg::texture_usage::depth_stencil});
                    auto pass = cmd->raster.render_to(
                        shaders::cube::target{.color = e.target.as_render_target_view().cleared(sees_through ? tg::vec4f(0, 0, 0, 0) : tg::vec4f(0.02f, 0.02f, 0.03f, 1.0f)),
                                              .depth_stencil = depth.as_depth_stencil_view().cleared(1.0f)});
                    pass.bind_pipeline(*pipeline);
                    pass.bind_vertex_buffers({vertices.as_vertex_buffer()});
                    pass.bind_index_buffer(indices.as_index_buffer());
                    pass.set_inline_constants(shaders::cube::constants{.view_projection = transform}.to_block());
                    pass.draw_indexed({.index_range = {.offset = 0, .size = cube_index_count}});
                }

                // filled from a native scope with legacy barriers, as dx12_context::wrap_dx12_texture's notes prescribe
                sg::backend::dx12::native_texture_access const reads[]
                    = {{.texture = e.target.raw(), .access = sg::access_flag::copy_read, .stages = sg::pipeline_stage_flag::copy}};
                {
                    auto const scope = sg::backend::dx12::dx12_native_scope::open(*cmd, reads);
                    auto* const image = e.images[index].texture;
                    auto to_copy = D3D12_RESOURCE_BARRIER{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
                    to_copy.Transition = {.pResource = image,
                                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                          .StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          .StateAfter = D3D12_RESOURCE_STATE_COPY_DEST};
                    scope.list()->ResourceBarrier(1, &to_copy);
                    scope.list()->CopyResource(image, scope.resource(e.target.raw()));
                    auto back = to_copy;
                    back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    back.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    scope.list()->ResourceBarrier(1, &back);
                }

                projection_views.push_back({.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
                                            .pose = view.pose,
                                            .fov = view.fov,
                                            .subImage = {.swapchain = e.swapchain,
                                                         .imageRect = {.offset = {0, 0}, .extent = {e.width, e.height}}}});
            }
            ctx->submit_command_list(cc::move(cmd));
            for (auto& e : xr.eyes)
            {
                auto const release_info = XrSwapchainImageReleaseInfo{.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(e.swapchain, &release_info);
            }
        }

        auto const camera_layer = XrCompositionLayerPassthroughFB{.type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB,
                                                                  .flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                                                                  .space = XR_NULL_HANDLE,
                                                                  .layerHandle = xr.passthrough_layer};
        auto const layer = XrCompositionLayerProjection{
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
            .layerFlags = sees_through ? XrCompositionLayerFlags(XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) : 0,
            .space = xr.space,
            .viewCount = u32(projection_views.size()),
            .views = projection_views.data()};
        auto layers = cc::vector<XrCompositionLayerBaseHeader const*>();
        if (sees_through)
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader const*>(&camera_layer));
        if (should_render)
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader const*>(&layer));
        auto const end_info = XrFrameEndInfo{.type = XR_TYPE_FRAME_END_INFO,
                                             .displayTime = frame_state.predictedDisplayTime,
                                             .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                                             .layerCount = u32(layers.size()),
                                             .layers = layers.empty() ? nullptr : layers.data()};
        xrEndFrame(xr.session, &end_info);

        ctx->advance_epoch();
        co_await ctx->epochs_in_flight_completion(2);
        if (++frames % 300 == 0)
            cc::println("xr: {} frames, {:.1f} fps", frames, double(frames) / (cc::current_time_steady_secs() - start));
    }

    ctx->advance_epoch();
    co_await ctx->idle_completion();
}
