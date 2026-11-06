#include <clean-core/string/format.hh>
#include <shaped-xr/impl/openxr.hh>
#include <shaped-xr/runtime_report.hh>

using namespace cc::primitive_defines;

namespace
{
[[nodiscard]] cc::any_error error_of(cc::string message)
{
    return cc::any_error(cc::move(message));
}
} // namespace

#if !SXR_HAS_OPENXR

cc::result<sxr::runtime_report> sxr::inspect_runtime()
{
    return cc::error(error_of("this build has no OpenXR loader (uv run extern/openxr/fetch-openxr.py)"));
}

#else

namespace
{
struct instance_guard
{
    XrInstance instance = XR_NULL_HANDLE;

    ~instance_guard()
    {
        if (instance != XR_NULL_HANDLE)
            xrDestroyInstance(instance);
    }
};

// what the system properties below answer for, and what those extensions depend on
constexpr char const* inspected_extensions[] = {
    XR_EXT_FUTURE_EXTENSION_NAME,
    XR_FB_SPATIAL_ENTITY_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_DISCOVERY_EXTENSION_NAME,
    XR_META_SPATIAL_ENTITY_PERSISTENCE_EXTENSION_NAME,
    XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME,
    XR_META_ENVIRONMENT_RAYCAST_EXTENSION_NAME,
    XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME,
    XR_EXT_SPATIAL_PLANE_TRACKING_EXTENSION_NAME,
    XR_EXT_SPATIAL_MESH_TRACKING_EXTENSION_NAME,
    XR_EXT_SPATIAL_ANCHOR_EXTENSION_NAME,
};

[[nodiscard]] bool contains(cc::vector<cc::string> const& names, char const* name)
{
    for (auto const& n : names)
        if (cc::string_view(n) == name)
            return true;
    return false;
}
} // namespace

cc::result<sxr::runtime_report> sxr::inspect_runtime()
{
    auto count = u32(0);
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr)))
        return cc::error(error_of("no OpenXR runtime is active (the Meta Horizon Link app or SteamVR sets one)"));
    auto offered = cc::vector<XrExtensionProperties>();
    for (auto i = u32(0); i < count; ++i)
        offered.push_back({.type = XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, offered.data());

    auto report = runtime_report();
    for (auto const& e : offered)
        report.extensions.push_back(cc::string(e.extensionName));

    // asking for what the runtime does not list fails the instance
    auto enabled = cc::vector<char const*>();
    for (auto const* name : inspected_extensions)
        if (contains(report.extensions, name))
            enabled.push_back(name);

    auto guard = instance_guard();
    auto const info = XrInstanceCreateInfo{
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo
        = {.applicationName = "shaped-xr inspect", .engineName = "shaped-core", .apiVersion = XR_API_VERSION_1_0},
        .enabledExtensionCount = u32(enabled.size()),
        .enabledExtensionNames = enabled.data()};
    if (auto const why = impl::failed(XR_NULL_HANDLE, xrCreateInstance(&info, &guard.instance), "xrCreateInstance");
        !why.empty())
        return cc::error(error_of(why));
    auto const instance = guard.instance;

    auto runtime = XrInstanceProperties{.type = XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance, &runtime);
    report.runtime_name = cc::string(runtime.runtimeName);
    report.runtime_version
        = cc::format("{}.{}.{}", XR_VERSION_MAJOR(runtime.runtimeVersion), XR_VERSION_MINOR(runtime.runtimeVersion),
                     XR_VERSION_PATCH(runtime.runtimeVersion));

    auto system_id = XrSystemId(XR_NULL_SYSTEM_ID);
    auto const system_info
        = XrSystemGetInfo{.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    if (XR_FAILED(xrGetSystem(instance, &system_info, &system_id)))
        return report;

    // a struct may be chained only for an extension that was enabled
    auto entity = XrSystemSpatialEntityPropertiesFB{.type = XR_TYPE_SYSTEM_SPATIAL_ENTITY_PROPERTIES_FB};
    auto discovery = XrSystemSpaceDiscoveryPropertiesMETA{.type = XR_TYPE_SYSTEM_SPACE_DISCOVERY_PROPERTIES_META};
    auto persistence = XrSystemSpacePersistencePropertiesMETA{.type = XR_TYPE_SYSTEM_SPACE_PERSISTENCE_PROPERTIES_META};
    auto depth = XrSystemEnvironmentDepthPropertiesMETA{.type = XR_TYPE_SYSTEM_ENVIRONMENT_DEPTH_PROPERTIES_META};
    auto raycast = XrSystemEnvironmentRaycastPropertiesMETA{.type = XR_TYPE_SYSTEM_ENVIRONMENT_RAYCAST_PROPERTIES_META};
    auto properties = XrSystemProperties{.type = XR_TYPE_SYSTEM_PROPERTIES};
    auto const chain = [&](char const* extension, auto& link)
    {
        if (!contains(report.extensions, extension))
            return;
        link.next = properties.next;
        properties.next = &link;
    };
    chain(XR_FB_SPATIAL_ENTITY_EXTENSION_NAME, entity);
    chain(XR_META_SPATIAL_ENTITY_DISCOVERY_EXTENSION_NAME, discovery);
    chain(XR_META_SPATIAL_ENTITY_PERSISTENCE_EXTENSION_NAME, persistence);
    chain(XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME, depth);
    chain(XR_META_ENVIRONMENT_RAYCAST_EXTENSION_NAME, raycast);
    if (auto const why
        = impl::failed(instance, xrGetSystemProperties(instance, system_id, &properties), "xrGetSystemProperties");
        !why.empty())
        return cc::error(error_of(why));
    report.system_name = cc::string(properties.systemName);

    auto const feature = [&](char const* name, XrBool32 is_supported)
    { report.features.push_back({.name = cc::string(name), .is_supported = is_supported == XR_TRUE}); };
    feature("spatial entities (FB)", entity.supportsSpatialEntity);
    feature("space discovery (META)", discovery.supportsSpaceDiscovery);
    feature("space persistence (META)", persistence.supportsSpacePersistence);
    feature("environment depth (META)", depth.supportsEnvironmentDepth);
    feature("environment depth hand removal (META)", depth.supportsHandRemoval);
    feature("environment raycast (META)", raycast.supportsEnvironmentRaycast);

    // the cross-vendor family answers by listing capabilities, not by a property struct
    auto capabilities = cc::vector<XrSpatialCapabilityEXT>();
    if (contains(report.extensions, XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME))
    {
        auto const enumerate
            = impl::proc<PFN_xrEnumerateSpatialCapabilitiesEXT>(instance, "xrEnumerateSpatialCapabilitiesEXT");
        auto capability_count = u32(0);
        if (enumerate != nullptr && XR_SUCCEEDED(enumerate(instance, system_id, 0, &capability_count, nullptr)))
        {
            for (auto i = u32(0); i < capability_count; ++i)
                capabilities.push_back(XR_SPATIAL_CAPABILITY_MAX_ENUM_EXT);
            enumerate(instance, system_id, capability_count, &capability_count, capabilities.data());
        }
    }
    auto const capability = [&](char const* name, XrSpatialCapabilityEXT wanted)
    {
        auto found = false;
        for (auto const c : capabilities)
            found = found || c == wanted;
        report.features.push_back({.name = cc::string(name), .is_supported = found});
    };
    capability("plane tracking (EXT)", XR_SPATIAL_CAPABILITY_PLANE_TRACKING_EXT);
    capability("mesh tracking (EXT)", XR_SPATIAL_CAPABILITY_MESH_TRACKING_EXT);
    capability("anchors (EXT)", XR_SPATIAL_CAPABILITY_ANCHOR_EXT);
    return report;
}

#endif
