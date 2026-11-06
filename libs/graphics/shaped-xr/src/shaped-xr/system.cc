#include <clean-core/common/utility.hh>
#include <clean-core/record/domain.hh>
#include <clean-core/record/log.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/all.hh>
#include <shaped-xr/impl/openxr.hh>


using namespace cc::primitive_defines;

namespace sxr
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "sxr");
}

namespace
{
[[nodiscard]] cc::any_error error_of(cc::string message)
{
    return cc::any_error(cc::move(message));
}
} // namespace

sxr::system::system(cc::unique_ptr<state> s) : _state(cc::move(s))
{
}
sxr::system::~system() = default;

#if !SXR_HAS_OPENXR

struct sxr::system::state
{
};

cc::result<cc::unique_ptr<sxr::system>> sxr::system::try_create(system_description const&)
{
    return cc::error(error_of("this build has no OpenXR loader (uv run extern/openxr/fetch-openxr.py)"));
}
cc::string_view sxr::system::runtime_name() const
{
    return {};
}
cc::string_view sxr::system::system_name() const
{
    return {};
}
sxr::graphics_api sxr::system::api() const
{
    return graphics_api::vulkan;
}
bool sxr::system::offers(capability) const
{
    return false;
}
bool sxr::system::offers_blend(blend_mode b) const
{
    return b == blend_mode::opaque;
}
cc::result<sg::context_handle> sxr::system::create_context() const
{
    return cc::error(error_of("no OpenXR"));
}
cc::result<cc::unique_ptr<sxr::session>> sxr::system::create_session(sg::context&, session_description const&)
{
    return cc::error(error_of("no OpenXR"));
}

#else

cc::string sxr::impl::failed(XrInstance instance, XrResult result, cc::string_view what)
{
    if (XR_SUCCEEDED(result))
        return {};
    char name[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance != XR_NULL_HANDLE)
        xrResultToString(instance, result, name);
    return cc::format("{} failed: {} ({})", what, name[0] != 0 ? name : "unknown", int(result));
}

cc::result<sxr::impl::system_answer> sxr::impl::read_system(XrInstance instance,
                                                            XrSystemId system,
                                                            cc::span<char const* const> enabled)
{
    auto entity = XrSystemSpatialEntityPropertiesFB{.type = XR_TYPE_SYSTEM_SPATIAL_ENTITY_PROPERTIES_FB};
    auto discovery = XrSystemSpaceDiscoveryPropertiesMETA{.type = XR_TYPE_SYSTEM_SPACE_DISCOVERY_PROPERTIES_META};
    auto persistence = XrSystemSpacePersistencePropertiesMETA{.type = XR_TYPE_SYSTEM_SPACE_PERSISTENCE_PROPERTIES_META};
    auto depth = XrSystemEnvironmentDepthPropertiesMETA{.type = XR_TYPE_SYSTEM_ENVIRONMENT_DEPTH_PROPERTIES_META};
    auto raycast = XrSystemEnvironmentRaycastPropertiesMETA{.type = XR_TYPE_SYSTEM_ENVIRONMENT_RAYCAST_PROPERTIES_META};
    auto properties = XrSystemProperties{.type = XR_TYPE_SYSTEM_PROPERTIES};

    // a struct may be chained only for an extension that was enabled
    auto const chain = [&](char const* extension, auto& link)
    {
        for (auto const* name : enabled)
            if (cc::string_view(name) == extension)
            {
                link.next = properties.next;
                properties.next = &link;
                return;
            }
    };
    chain(XR_FB_SPATIAL_ENTITY_EXTENSION_NAME, entity);
    chain(XR_META_SPATIAL_ENTITY_DISCOVERY_EXTENSION_NAME, discovery);
    chain(XR_META_SPATIAL_ENTITY_PERSISTENCE_EXTENSION_NAME, persistence);
    chain(XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME, depth);
    chain(XR_META_ENVIRONMENT_RAYCAST_EXTENSION_NAME, raycast);
    if (auto const why = failed(instance, xrGetSystemProperties(instance, system, &properties), "xrGetSystemProperties");
        !why.empty())
        return cc::error(error_of(why));

    return system_answer{.name = cc::string(properties.systemName),
                         .support = {.spatial_entity = entity.supportsSpatialEntity == XR_TRUE,
                                     .space_discovery = discovery.supportsSpaceDiscovery == XR_TRUE,
                                     .space_persistence = persistence.supportsSpacePersistence == XR_TRUE,
                                     .environment_depth = depth.supportsEnvironmentDepth == XR_TRUE,
                                     .hand_removal = depth.supportsHandRemoval == XR_TRUE,
                                     .environment_raycast = raycast.supportsEnvironmentRaycast == XR_TRUE}};
}

sxr::system::state::~state()
{
    if (instance != XR_NULL_HANDLE)
        xrDestroyInstance(instance);
}

cc::result<cc::unique_ptr<sxr::system>> sxr::system::try_create(system_description const& desc)
{
    auto s = cc::make_unique<state>();
    s->api = desc.api;

    char const* binding = nullptr;
#if SXR_HAS_VULKAN
    if (desc.api == graphics_api::vulkan)
        binding = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
#endif
#if SXR_HAS_DX12
    if (desc.api == graphics_api::dx12)
        binding = XR_KHR_D3D12_ENABLE_EXTENSION_NAME;
#endif
    if (binding == nullptr)
        return cc::error(
            error_of(cc::format("this build has no {} backend", desc.api == graphics_api::vulkan ? "vulkan" : "dx12")));

    // what the runtime offers decides what may be asked for, since asking for more fails the instance
    auto count = u32(0);
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr)))
        return cc::error(error_of("no OpenXR runtime is active (the Meta Horizon Link app or SteamVR sets one)"));
    auto offered = cc::vector<XrExtensionProperties>();
    for (auto i = u32(0); i < count; ++i)
        offered.push_back({.type = XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, offered.data());
    auto const offers = [&](char const* name)
    {
        for (auto const& e : offered)
            if (cc::string_view(e.extensionName) == name)
                return true;
        return false;
    };
    if (!offers(binding))
        return cc::error(error_of(cc::format("the active OpenXR runtime offers no {}", binding)));

    auto extensions = cc::vector<char const*>{binding};
    s->offers_depth_layer = offers(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    if (s->offers_depth_layer)
        extensions.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);

    // a capability missing one extension is left out whole, since what it needs comes together or not at all
    auto listed = cc::vector<capability>();
    for (auto const c : desc.wants)
    {
        auto has_all = true;
        for (auto const* name : impl::required_extensions(c))
            has_all = has_all && offers(name);
        if (has_all)
            listed.push_back(c);
        else
            CC_LOG_INFO("{} is wanted, but the runtime does not list what it needs", impl::name_of(c));
    }
    for (auto const* name : impl::extensions_of(listed))
        extensions.push_back(name);

    auto info = XrInstanceCreateInfo{.type = XR_TYPE_INSTANCE_CREATE_INFO,
                                     .enabledExtensionCount = u32(extensions.size()),
                                     .enabledExtensionNames = extensions.data()};
    auto const copy_name = [](char* to, isize capacity, cc::string_view from)
    {
        auto const n = cc::min(from.size(), capacity - 1); // the runtime reads a terminated string
        for (auto i = isize(0); i < n; ++i)
            to[i] = from[i];
        to[n] = 0;
    };
    copy_name(info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, desc.application_name);
    copy_name(info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "shaped-core");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    if (auto const why = impl::failed(XR_NULL_HANDLE, xrCreateInstance(&info, &s->instance), "xrCreateInstance");
        !why.empty())
        return cc::error(error_of(why));

    auto const system_info
        = XrSystemGetInfo{.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    if (auto const why = impl::failed(s->instance, xrGetSystem(s->instance, &system_info, &s->system), "xrGetSystem");
        !why.empty())
        return cc::error(error_of(cc::format("{}: no headset is connected", why)));

    auto runtime = XrInstanceProperties{.type = XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(s->instance, &runtime);
    s->runtime_name = cc::string(runtime.runtimeName);
    auto answer = impl::read_system(s->instance, s->system, extensions);
    if (answer.has_error())
        return cc::error(cc::move(answer).error());
    s->system_name = cc::move(answer.value().name);
    for (auto const c : listed)
        if (impl::is_supported(c, answer.value().support))
            s->offered.push_back(c);
        else
            CC_LOG_INFO("{} is listed, but this headset says it does not have it", impl::name_of(c));

    auto blend_count = u32(0);
    xrEnumerateEnvironmentBlendModes(s->instance, s->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blend_count,
                                     nullptr);
    auto listed_blends = cc::vector<XrEnvironmentBlendMode>();
    for (auto i = u32(0); i < blend_count; ++i)
        listed_blends.push_back(XR_ENVIRONMENT_BLEND_MODE_MAX_ENUM);
    xrEnumerateEnvironmentBlendModes(s->instance, s->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, blend_count,
                                     &blend_count, listed_blends.data());
    for (auto const b : listed_blends)
        if (b == XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
            s->runtime_blends.push_back(blend_mode::opaque);
        else if (b == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE)
            s->runtime_blends.push_back(blend_mode::additive);
        else if (b == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
            s->runtime_blends.push_back(blend_mode::alpha_blend);

    CC_LOG_INFO("{} on {}", s->system_name, s->runtime_name);
    return cc::make_unique<system>(cc::move(s));
}

cc::string_view sxr::system::runtime_name() const
{
    return _state->runtime_name;
}
cc::string_view sxr::system::system_name() const
{
    return _state->system_name;
}
sxr::graphics_api sxr::system::api() const
{
    return _state->api;
}
bool sxr::system::offers(capability c) const
{
    for (auto const offered : _state->offered)
        if (offered == c)
            return true;
    return false;
}

bool sxr::system::offers_blend(blend_mode b) const
{
    return impl::choose_blend(b, _state->runtime_blends, offers(capability::passthrough)).shown == b;
}

cc::result<sg::context_handle> sxr::system::create_context() const
{
    auto const instance = _state->instance;
    auto const system_id = _state->system;
#if SXR_HAS_VULKAN
    if (_state->api == graphics_api::vulkan)
    {
        // the requirements must be asked before the instance, and sg creates at a version the range must include
        auto requirements = XrGraphicsRequirementsVulkan2KHR{.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        auto const get_requirements
            = impl::proc<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance, "xrGetVulkanGraphicsRequirements2KHR");
        if (auto const why = impl::failed(instance, get_requirements(instance, system_id, &requirements),
                                          "xrGetVulkanGraphicsRequirements2KHR");
            !why.empty())
            return cc::error(error_of(why));
        auto const wanted = XR_MAKE_VERSION(VK_API_VERSION_MAJOR(sg::backend::vulkan::required_api_version),
                                            VK_API_VERSION_MINOR(sg::backend::vulkan::required_api_version), 0);
        if (wanted < requirements.minApiVersionSupported)
            return cc::error(error_of("the runtime needs a newer Vulkan than the one sg creates"));

        auto config = sg::backend::vulkan::vulkan_config();
        auto const create_instance = impl::proc<PFN_xrCreateVulkanInstanceKHR>(instance, "xrCreateVulkanInstanceKHR");
        auto const physical_device
            = impl::proc<PFN_xrGetVulkanGraphicsDevice2KHR>(instance, "xrGetVulkanGraphicsDevice2KHR");
        auto const create_device = impl::proc<PFN_xrCreateVulkanDeviceKHR>(instance, "xrCreateVulkanDeviceKHR");
        config.create_instance = [=](VkInstanceCreateInfo const& info, VkInstance& out) -> VkResult
        {
            auto const xr_info = XrVulkanInstanceCreateInfoKHR{.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
                                                               .systemId = system_id,
                                                               .pfnGetInstanceProcAddr = &vkGetInstanceProcAddr,
                                                               .vulkanCreateInfo = &info};
            auto vk_result = VK_SUCCESS;
            if (XR_FAILED(create_instance(instance, &xr_info, &out, &vk_result)))
                return VK_ERROR_INITIALIZATION_FAILED;
            return vk_result;
        };
        config.choose_physical_device = [=](VkInstance vk_instance) -> VkPhysicalDevice
        {
            auto const xr_info = XrVulkanGraphicsDeviceGetInfoKHR{.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
                                                                  .systemId = system_id,
                                                                  .vulkanInstance = vk_instance};
            auto device = VkPhysicalDevice(VK_NULL_HANDLE);
            physical_device(instance, &xr_info, &device);
            return device;
        };
        config.create_device = [=](VkPhysicalDevice device, VkDeviceCreateInfo const& info, VkDevice& out) -> VkResult
        {
            auto const xr_info = XrVulkanDeviceCreateInfoKHR{.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
                                                             .systemId = system_id,
                                                             .pfnGetInstanceProcAddr = &vkGetInstanceProcAddr,
                                                             .vulkanPhysicalDevice = device,
                                                             .vulkanCreateInfo = &info};
            auto vk_result = VK_SUCCESS;
            if (XR_FAILED(create_device(instance, &xr_info, &out, &vk_result)))
                return VK_ERROR_INITIALIZATION_FAILED;
            return vk_result;
        };
        return sg::create_vulkan_context(config);
    }
#endif
#if SXR_HAS_DX12
    if (_state->api == graphics_api::dx12)
    {
        auto requirements = XrGraphicsRequirementsD3D12KHR{.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        auto const get_requirements
            = impl::proc<PFN_xrGetD3D12GraphicsRequirementsKHR>(instance, "xrGetD3D12GraphicsRequirementsKHR");
        if (auto const why = impl::failed(instance, get_requirements(instance, system_id, &requirements),
                                          "xrGetD3D12GraphicsRequirementsKHR");
            !why.empty())
            return cc::error(error_of(why));
        auto const luid = u64(requirements.adapterLuid.LowPart) | (u64(u32(requirements.adapterLuid.HighPart)) << 32);
        return sg::create_dx12_context({.adapter_luid = luid, .minimum_feature_level = requirements.minFeatureLevel});
    }
#endif
    return cc::error(error_of("this build has no backend for the system's graphics API"));
}

#endif
