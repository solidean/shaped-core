// vulkan context bring-up: optional validation, instance, physical-device selection, logical device + graphics queue.
// Split off from the other vulkan_context bodies because it grows with every device feature opted into.

#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh> // CC_DEFER
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>


namespace sg::backend::vulkan
{
namespace
{
char const* const k_validation_layer = "VK_LAYER_KHRONOS_validation";

// Validation messages routed to stderr.
// Runs on whatever thread the loader raises the message from.
// Always returns VK_FALSE — never aborts the offending call.
VKAPI_ATTR VkBool32 VKAPI_CALL debug_messenger_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                        VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                                        VkDebugUtilsMessengerCallbackDataEXT const* data,
                                                        void* /*user_data*/)
{
    char const* level = "message";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        level = "error";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        level = "warning";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        level = "info";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)
        level = "verbose";
    // The validation layer already told us how bad it is, so the level maps straight across rather than flattening
    // every message onto one.
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        CC_LOG_ERROR("validation: {}", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        CC_LOG_WARNING("validation: {}", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        CC_LOG_INFO("validation: {}", data->pMessage);
    else
        CC_LOG_DEBUG("validation: {}", data->pMessage);
    (void)level;
    return VK_FALSE;
}

// Shared by the instance pNext (to catch create/destroy-time messages) and the standalone messenger.
VkDebugUtilsMessengerCreateInfoEXT make_debug_messenger_info()
{
    return VkDebugUtilsMessengerCreateInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT //
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |    //
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | //
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = &debug_messenger_callback,
    };
}

bool validation_layer_available()
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    auto layers = cc::vector<VkLayerProperties>::create_uninitialized(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (auto const& l : layers)
        if (cc::string_view(l.layerName) == k_validation_layer)
            return true;
    return false;
}

bool debug_utils_extension_available()
{
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    auto exts = cc::vector<VkExtensionProperties>::create_uninitialized(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    for (auto const& e : exts)
        if (cc::string_view(e.extensionName) == VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
            return true;
    return false;
}

// First queue family with graphics support, or false if the device has none.
bool find_graphics_queue_family(VkPhysicalDevice dev, u32& out_index)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    auto families = cc::vector<VkQueueFamilyProperties>::create_uninitialized(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            out_index = i;
            return true;
        }
    return false;
}

// Even `unsuitable` beats no device at all, so a lone less-ideal device is still picked.
// `prefer_software` lifts CPU devices (lavapipe) to the top; otherwise a discrete GPU wins and CPU comes last.
int device_type_rank(VkPhysicalDeviceType type, bool prefer_software)
{
    // Selection tiers; higher wins.
    enum : int
    {
        unsuitable = 0,
        acceptable = 1,
        good = 2,
        ideal = 3,
    };

    if (prefer_software)
    {
        // A CPU device is ideal; a discrete GPU is an acceptable fallback; everything else unsuitable.
        switch (type)
        {
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return ideal;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return acceptable;
        default:
            return unsuitable;
        }
    }

    // Discrete GPU is ideal, then integrated, then virtual; CPU / other last.
    switch (type)
    {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return ideal;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return good;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return acceptable;
    default: // CPU / OTHER
        return unsuitable;
    }
}

struct selected_physical_device
{
    VkPhysicalDevice device;
    u32 queue_family;
};

// Highest-ranked physical device that exposes a graphics queue, or nullopt if none qualifies (no
// devices at all, or none with a graphics queue).
cc::optional<selected_physical_device> pick_physical_device(VkInstance instance, bool prefer_software)
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    auto devices = cc::vector<VkPhysicalDevice>::create_uninitialized(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    cc::optional<selected_physical_device> best;
    int best_rank = -1;
    for (auto device : devices)
    {
        u32 family = 0;
        if (!find_graphics_queue_family(device, family))
            continue;

        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(device, &props);
        int const rank = device_type_rank(props.deviceType, prefer_software);
        if (rank > best_rank)
        {
            best_rank = rank;
            best = selected_physical_device{.device = device, .queue_family = family};
        }
    }
    return best;
}

/// What vulkan says about the physical device that was picked.
///
/// `driverVersion` is vendor-encoded and vulkan defines no portable decomposition, so it is recorded as the raw
/// number — which is enough for the only thing sg::adapter_info promises about it, equality.
sg::adapter_info describe_adapter(VkPhysicalDevice device)
{
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(device, &props);

    auto info = sg::adapter_info();
    info.name = cc::string(props.deviceName); // a NUL-terminated char array, not length-prefixed
    info.vendor_id = u32(props.vendorID);
    info.device_id = u32(props.deviceID);
    info.driver_version = cc::format("{}", props.driverVersion);
    info.is_software = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    return info;
}

void destroy_debug_messenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger)
{
    if (messenger == VK_NULL_HANDLE)
        return;
    auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (fn)
        fn(instance, messenger, nullptr);
}

// The Vulkan version sg requires of a device, and the floor every feature below is core in.
// Hardcoded rather than probed per capability: a branch taken on nobody's machine is a path nobody exercises,
// so a device under the floor is refused at creation with one legible error instead of degrading silently.
constexpr u32 k_required_api_version = VK_API_VERSION_1_3;

// The ray-tracing device extensions, which are optional above the floor.
// A device without them still comes up; cmd.raytracing.is_supported() then answers false.
constexpr char const* k_raytracing_extensions[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
};

// Whether `dev` advertises every extension in `names`.
bool device_extensions_available(VkPhysicalDevice dev, cc::span<char const* const> names)
{
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    auto props = cc::vector<VkExtensionProperties>::create_uninitialized(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, props.data());

    for (auto const* wanted : names)
    {
        bool found = false;
        for (auto const& p : props)
            if (cc::string_view(p.extensionName) == cc::string_view(wanted))
            {
                found = true;
                break;
            }
        if (!found)
            return false;
    }
    return true;
}

// Whether the device supports timeline semaphores — the epoch system's core sync primitive.
// Core in the 1.2 baseline, but still a feature bit a device may not expose.
// The first thing sg requires that `dev` does not have, or an empty view when it satisfies the floor.
// Naming the missing capability is the point: "this GPU is too old" is not something a caller can act on.
cc::string_view missing_required_capability(VkPhysicalDevice dev)
{
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(dev, &props);
    if (props.apiVersion < k_required_api_version)
        return "Vulkan 1.3";

    VkPhysicalDeviceVulkan13Features vk13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features vk12
        = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &vk13};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vk12};
    vkGetPhysicalDeviceFeatures2(dev, &features);

    // The epoch system rests on timeline semaphores; barriers on synchronization2; the raster scope on dynamic
    // rendering; bindless arrays on the three descriptor-indexing bits; acceleration structures on device addresses.
    if (vk12.timelineSemaphore != VK_TRUE)
        return "timelineSemaphore";
    if (vk13.synchronization2 != VK_TRUE)
        return "synchronization2";
    if (vk13.dynamicRendering != VK_TRUE)
        return "dynamicRendering";
    if (vk12.runtimeDescriptorArray != VK_TRUE)
        return "runtimeDescriptorArray";
    if (vk12.descriptorBindingPartiallyBound != VK_TRUE)
        return "descriptorBindingPartiallyBound";
    if (vk12.descriptorBindingUpdateUnusedWhilePending != VK_TRUE)
        return "descriptorBindingUpdateUnusedWhilePending";
    if (vk12.bufferDeviceAddress != VK_TRUE)
        return "bufferDeviceAddress";
    return {};
}

// Creates a timeline semaphore starting at `initial_value`.
// Read with vkGetSemaphoreCounterValue and waited on with vkWaitSemaphores — no host event needed.
VkResult create_timeline_semaphore(VkDevice device, u64 initial_value, VkSemaphore& out)
{
    auto const type_info = VkSemaphoreTypeCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = initial_value,
    };
    auto const info = VkSemaphoreCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type_info,
    };
    return vkCreateSemaphore(device, &info, nullptr, &out);
}
} // namespace
} // namespace sg::backend::vulkan

namespace sg
{
cc::result<context_handle> create_vulkan_context(backend::vulkan::vulkan_config const& config)
{
    using namespace sg::backend::vulkan;

    // Validation is best-effort: enabled only when both the layer and VK_EXT_debug_utils are present.
    bool const enable_validation
        = config.enable_validation_layers && validation_layer_available() && debug_utils_extension_available();

    auto const app = VkApplicationInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "shaped-graphics",
        .apiVersion = k_required_api_version, // the floor every feature below is core in; see k_required_api_version
    };

    cc::vector<char const*> layers;
    cc::vector<char const*> extensions;
    if (enable_validation)
    {
        layers.push_back(k_validation_layer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    auto const dbg_info = make_debug_messenger_info();

    auto const instance_info = VkInstanceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = enable_validation ? &dbg_info : nullptr, // catches messages during instance create/destroy too
        .pApplicationInfo = &app,
        .enabledLayerCount = u32(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = u32(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };

    VkInstance instance = VK_NULL_HANDLE;
    if (VkResult r = vkCreateInstance(&instance_info, nullptr, &instance); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateInstance failed");

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (enable_validation)
    {
        auto create_fn
            = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (create_fn)
            create_fn(instance, &dbg_info, nullptr, &messenger); // best-effort; ignore failure
    }

    // Physical device selection: rank every device that exposes a graphics queue, keep the best.
    auto const picked = pick_physical_device(instance, config.prefer_software_device);
    if (!picked.has_value())
    {
        destroy_debug_messenger(instance, messenger);
        vkDestroyInstance(instance, nullptr);
        return cc::error("no Vulkan device with a graphics queue found");
    }
    auto const best_device = picked.value().device;
    auto const best_family = picked.value().queue_family;

    // Everything created from here is owned by this function until the context takes it, so one guard unwinds
    // whatever exists at the point of an early return.
    // It disarms once the context owns the handles, which is why the success path frees nothing.
    VkDevice device = VK_NULL_HANDLE;
    VkSemaphore epoch_timeline = VK_NULL_HANDLE;
    VkSemaphore submission_timeline = VK_NULL_HANDLE;
    bool owned_by_context = false;
    CC_DEFER
    {
        if (owned_by_context)
            return;
        if (submission_timeline != VK_NULL_HANDLE)
            vkDestroySemaphore(device, submission_timeline, nullptr);
        if (epoch_timeline != VK_NULL_HANDLE)
            vkDestroySemaphore(device, epoch_timeline, nullptr);
        if (device != VK_NULL_HANDLE)
            vkDestroyDevice(device, nullptr);
        destroy_debug_messenger(instance, messenger);
        vkDestroyInstance(instance, nullptr);
    };

    // Refuse a device under the floor, naming what it is missing.
    if (auto const missing = missing_required_capability(best_device); !missing.empty())
        return cc::error(cc::format("selected Vulkan device does not support {}", missing));

    // Ray tracing is optional above the floor: enable the extensions where the device has them, and record the
    // answer so cmd.raytracing.is_supported() reports the device rather than a hardcoded false.
    bool const raytracing_supported = device_extensions_available(best_device, k_raytracing_extensions);

    cc::vector<char const*> device_extensions;
    if (raytracing_supported)
        for (auto const* name : k_raytracing_extensions)
            device_extensions.push_back(name);

    // Logical device with a single graphics queue.
    // Every feature is enabled up front, whether or not the milestone using it has landed: a feature costs nothing
    // unused, and enabling them one at a time means re-editing this chain for each.
    float const queue_priority = 1.0f;
    auto const queue_info = VkDeviceQueueCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = best_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };

    auto accel_features = VkPhysicalDeviceAccelerationStructureFeaturesKHR{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .accelerationStructure = VK_TRUE,
    };
    auto rt_pipeline_features = VkPhysicalDeviceRayTracingPipelineFeaturesKHR{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = &accel_features,
        .rayTracingPipeline = VK_TRUE,
    };
    auto vk13_features = VkPhysicalDeviceVulkan13Features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = raytracing_supported ? static_cast<void*>(&rt_pipeline_features) : nullptr,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    auto vk12_features = VkPhysicalDeviceVulkan12Features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &vk13_features,
        .descriptorBindingUpdateUnusedWhilePending = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
    };
    auto const device_info = VkDeviceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &vk12_features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = u32(device_extensions.size()),
        .ppEnabledExtensionNames = device_extensions.data(),
    };

    if (VkResult r = vkCreateDevice(best_device, &device_info, nullptr, &device); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateDevice failed");

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, best_family, 0, &queue);

    // Two timeline semaphores on the one queue: the epoch timeline drives reclamation, the submission timeline answers per-list completion.
    // Each starts at first-1, so nothing reads as complete before the first signal.
    if (VkResult r = create_timeline_semaphore(device, u64(sg::epoch::first) - 1, epoch_timeline); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateSemaphore (epoch timeline) failed");

    if (VkResult r = create_timeline_semaphore(device, u64(sg::submission_token::first) - 1, submission_timeline);
        r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateSemaphore (submission timeline) failed");

    auto ctx = std::make_shared<vulkan_context>(instance, best_device, device, queue, best_family, epoch_timeline,
                                                submission_timeline, messenger);
    ctx->set_adapter_info(describe_adapter(best_device));
    ctx->set_raytracing_supported(raytracing_supported);
    owned_by_context = true;
    return context_handle(cc::move(ctx));
}
} // namespace sg
