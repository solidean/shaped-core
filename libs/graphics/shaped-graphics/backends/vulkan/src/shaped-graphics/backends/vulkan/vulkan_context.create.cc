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
#include <shaped-graphics/backends/vulkan/vulkan_driver_lock.hh>


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
                                                        void* user_data)
{
    auto mapped = vulkan_message_severity::verbose;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        mapped = vulkan_message_severity::error;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        mapped = vulkan_message_severity::warning;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        mapped = vulkan_message_severity::info;

    // The messenger created alongside the instance carries no context yet, so its create-time messages go to the log.
    // The standalone one created after the context carries it, which is what lets a test fail on a validation message.
    if (auto* const ctx = static_cast<vulkan_context*>(user_data); ctx != nullptr)
        ctx->dispatch_validation_message(mapped, cc::string_view(data->pMessage));
    else if (mapped == vulkan_message_severity::error)
        CC_LOG_ERROR("validation: {}", data->pMessage);
    else if (mapped == vulkan_message_severity::warning)
        CC_LOG_WARNING("validation: {}", data->pMessage);
    else if (mapped == vulkan_message_severity::info)
        CC_LOG_INFO("validation: {}", data->pMessage);
    else
        CC_LOG_DEBUG("validation: {}", data->pMessage);
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

// Whether `layer` offers `name` as an instance extension; a null layer asks the loader itself.
// The layer form is what finds an extension a layer implements rather than the driver — VK_EXT_validation_features
// is only ever listed under the validation layer.
bool instance_extension_available(cc::string_view name, char const* layer = nullptr)
{
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(layer, &count, nullptr);
    auto exts = cc::vector<VkExtensionProperties>::create_uninitialized(count);
    vkEnumerateInstanceExtensionProperties(layer, &count, exts.data());
    for (auto const& e : exts)
        if (cc::string_view(e.extensionName) == name)
            return true;
    return false;
}

bool debug_utils_extension_available()
{
    return instance_extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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

/// The family async transfers should run on, and how many queues it can give.
///
/// Preference order: a transfer family with no graphics — a real DMA engine, which copies without competing with
/// rendering — then any non-graphics family that can transfer, then the graphics family itself.
///
/// **Two queues, not one.** A wait stalls everything behind it in a queue's FIFO, so an upload deferred behind a
/// graphics submission would hold up an unrelated download queued after it.
/// Where the family cannot give two, both directions share one and that stall is the cost; where there is no
/// transfer family at all, they share the graphics queue and async transfer is asynchronous only in the CPU sense.
/// Reporting which case applies is what keeps a surprise-free timing story: see vulkan_context::has_dedicated_transfer_queue.
u32 find_transfer_queue_family(VkPhysicalDevice dev, u32 graphics_family, u32& out_queue_count)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    auto families = cc::vector<VkQueueFamilyProperties>::create_uninitialized(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());

    auto const can_transfer = [&](uint32_t i)
    {
        // A graphics or compute family transfers implicitly, whether or not it sets the bit.
        return (families[i].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0;
    };

    for (uint32_t i = 0; i < count; ++i)
        if (can_transfer(i) && (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0)
        {
            out_queue_count = families[i].queueCount;
            return i;
        }
    for (uint32_t i = 0; i < count; ++i)
        if (i != graphics_family && can_transfer(i))
        {
            out_queue_count = families[i].queueCount;
            return i;
        }

    out_queue_count = families[graphics_family].queueCount;
    return graphics_family;
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

    // Vulkan reports heaps rather than a board size, so the card's memory is the sum of the DEVICE_LOCAL ones.
    // On an integrated GPU every heap is host memory and the sum is 0, which is the honest answer there.
    VkPhysicalDeviceMemoryProperties memory = {};
    vkGetPhysicalDeviceMemoryProperties(device, &memory);

    auto dedicated = i64(0);
    for (u32 i = 0; i < memory.memoryHeapCount; ++i)
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            dedicated += i64(memory.memoryHeaps[i].size);
    info.dedicated_video_memory_bytes = dedicated;

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

// Descriptor buffers back the whole bind path: a binding group is a range of plain memory a shader reads through a
// bound address, which is what lets a staging group snapshot by copying bytes rather than re-writing every descriptor.
// Required rather than probed, for the same reason the version floor is — see k_required_api_version.
//
// This is the older spelling of the feature.
// VK_EXT_descriptor_heap supersedes it — vulkan_core.h defines the descriptor-buffer capture-replay bit as an alias of
// the descriptor-heap one — and is where this should end up.
// RADV does not expose descriptor_heap yet, though, and requiring it would mean the backend cannot create a device on
// the machine it is developed on.
constexpr char const* k_descriptor_buffer_extension = VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME;

// What makes an *empty* descriptor writable.
// sg has two of them — a vacant array element, and the null acceleration structure every ray misses — and both are
// contract rather than convenience, so this is required alongside descriptor_buffer rather than probed.
// Without nullDescriptor a zeroed descriptor is undefined rather than empty, and vkGetDescriptorEXT rejects a null
// acceleration-structure address outright.
constexpr char const* k_robustness2_extension = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;

// The ray-tracing device extensions, which are optional above the floor.
// A device without them still comes up; cmd.raytracing.is_supported() then answers false.
constexpr char const* k_raytracing_extensions[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    // Ray query is in the required set rather than probed separately, because DXC emits the RayQueryKHR capability
    // into every ray-tracing SPIR-V module it produces, used or not.
    // So a device without it could not load any shader our own toolchain compiles, which makes "has ray tracing" and
    // "has ray query" the same question for this backend.
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
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

    // Excludes every ray-tracing pipeline build in the process for the duration; see vulkan_driver_lock.hh.
    // Held across instance AND device creation, since the deadlock is against either.
    scoped_device_lifecycle const driver_guard;

    // Validation is best-effort: enabled only when both the layer and VK_EXT_debug_utils are present.
    bool const enable_validation
        = config.enable_validation_layers && validation_layer_available() && debug_utils_extension_available();

    // Synchronization validation rides the same layer but is requested separately, through an extension the layer
    // itself implements rather than the driver.
    bool const enable_sync_validation
        = enable_validation && config.enable_sync_validation
       && instance_extension_available(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME, k_validation_layer);

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
    if (enable_sync_validation)
        extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);

    // Presentation extensions, each enabled only where the loader has it.
    // An instance extension that is not there fails instance creation outright, and a context that cannot present is
    // still a usable context.
    // Which of them a swapchain then needs is decided per swapchain: headless takes the headless surface, a window
    // takes its platform's.
    bool const has_surface = instance_extension_available(VK_KHR_SURFACE_EXTENSION_NAME);
    bool const has_headless_surface = has_surface && instance_extension_available(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    if (has_surface)
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    if (has_headless_surface)
        extensions.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    // One optional extension per windowing system this build can compile a surface call for.
    // Enabled only where the loader has it, since an instance extension that is not there fails creation outright.
    bool platform_surface[sg::window_platform_count] = {};
    auto const enable_platform_surface = [&](sg::window_platform platform, char const* name)
    {
        if (!has_surface || !instance_extension_available(name))
            return;
        platform_surface[int(platform)] = true;
        extensions.push_back(name);
    };
#ifdef VK_USE_PLATFORM_WIN32_KHR
    enable_platform_surface(sg::window_platform::win32, VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
    enable_platform_surface(sg::window_platform::xlib, VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#endif
#ifdef VK_USE_PLATFORM_XCB_KHR
    enable_platform_surface(sg::window_platform::xcb, VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
    enable_platform_surface(sg::window_platform::wayland, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#endif

    auto const dbg_info = make_debug_messenger_info();

    // Chained ahead of the messenger rather than instead of it, so sync-validation findings arrive through the same
    // callback as every other message and fail a test the same way.
    VkValidationFeatureEnableEXT const sync_features[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    auto const validation_features = VkValidationFeaturesEXT{
        .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .pNext = &dbg_info,
        .enabledValidationFeatureCount = 1,
        .pEnabledValidationFeatures = sync_features,
    };

    void const* instance_pnext = nullptr;
    if (enable_sync_validation)
        instance_pnext = &validation_features;
    else if (enable_validation)
        instance_pnext = &dbg_info;

    auto const instance_info = VkInstanceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = instance_pnext, // catches messages during instance create/destroy too
        .pApplicationInfo = &app,
        .enabledLayerCount = u32(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = u32(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };

    VkInstance instance = VK_NULL_HANDLE;
    if (VkResult r = vkCreateInstance(&instance_info, nullptr, &instance); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateInstance failed");

    // The standalone messenger is created after the context below, so it can carry it as user data and a caller can
    // redirect messages with set_message_callback.
    // Instance create/destroy messages are already covered by dbg_info on the instance's pNext.
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

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

    for (auto const* name : {k_descriptor_buffer_extension, k_robustness2_extension})
    {
        char const* const names[] = {name};
        if (!device_extensions_available(best_device, names))
            return cc::error(cc::format("selected Vulkan device does not support {}", name));
    }

    // Ray tracing is optional above the floor: enable the extensions where the device has them, and record the
    // answer so cmd.raytracing.is_supported() reports the device rather than a hardcoded false.
    bool raytracing_supported = device_extensions_available(best_device, k_raytracing_extensions);

    cc::vector<char const*> device_extensions;
    device_extensions.push_back(k_descriptor_buffer_extension);
    device_extensions.push_back(k_robustness2_extension);

    // The device half of presentation.
    // Optional: a device without it simply cannot create a swapchain.
    char const* const swapchain_names[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    bool const swapchain_supported = has_surface && device_extensions_available(best_device, swapchain_names);
    if (swapchain_supported)
        device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (raytracing_supported)
        for (auto const* name : k_raytracing_extensions)
            device_extensions.push_back(name);

    // Logical device with a single graphics queue.
    // Every feature is enabled up front, whether or not the milestone using it has landed: a feature costs nothing
    // unused, and enabling them one at a time means re-editing this chain for each.
    // The transfer family and how many of its queues async transfer gets: two where it can, one otherwise.
    u32 transfer_family_queue_count = 0;
    u32 const transfer_family = find_transfer_queue_family(best_device, best_family, transfer_family_queue_count);
    bool const transfer_shares_graphics_family = transfer_family == best_family;
    u32 transfer_queue_count = transfer_family_queue_count >= 2 ? 2u : 1u;
    if (transfer_shares_graphics_family)
    {
        // The graphics family already spends one queue on rendering, so only what is left over is available.
        transfer_queue_count = transfer_family_queue_count >= 3 ? 2u : (transfer_family_queue_count >= 2 ? 1u : 0u);
    }

    float const queue_priority = 1.0f;
    auto const queue_info = VkDeviceQueueCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = best_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };

    auto ray_query_features = VkPhysicalDeviceRayQueryFeaturesKHR{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
        .rayQuery = VK_TRUE,
    };
    auto accel_features = VkPhysicalDeviceAccelerationStructureFeaturesKHR{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &ray_query_features,
        .accelerationStructure = VK_TRUE,
    };
    auto rt_pipeline_features = VkPhysicalDeviceRayTracingPipelineFeaturesKHR{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = &accel_features,
        .rayTracingPipeline = VK_TRUE,
    };
    auto robustness2_features = VkPhysicalDeviceRobustness2FeaturesEXT{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .pNext = raytracing_supported ? static_cast<void*>(&rt_pipeline_features) : nullptr,
        // Only nullDescriptor: robustBufferAccess2 and robustImageAccess2 define what an out-of-bounds access
        // returns, which sg does not promise and which costs performance to guarantee.
        .nullDescriptor = VK_TRUE,
    };
    auto descriptor_buffer_features = VkPhysicalDeviceDescriptorBufferFeaturesEXT{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
        .pNext = &robustness2_features,
        .descriptorBuffer = VK_TRUE,
    };
    auto vk13_features = VkPhysicalDeviceVulkan13Features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &descriptor_buffer_features,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    auto vk12_features = VkPhysicalDeviceVulkan12Features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &vk13_features,
        .descriptorBindingUpdateUnusedWhilePending = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE,
        // How the query system resets a pool: vkCmdResetQueryPool cannot be recorded inside a render-pass instance,
        // and a timestamp legitimately can be.
        // See vulkan_query.hh.
        .hostQueryReset = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
    };
    // One create-info per family: the graphics one always, plus the transfer family when it is a different one.
    // A shared family asks for its extra queues on the single info instead, which Vulkan requires — two infos for one
    // family is invalid.
    // Three entries covers every case: the shared family asks for at most 1 graphics + 2 transfer queues.
    float const transfer_priorities[3] = {1.0f, 1.0f, 1.0f};
    cc::vector<VkDeviceQueueCreateInfo> queue_infos;
    if (transfer_shares_graphics_family)
    {
        auto shared = queue_info;
        shared.queueCount = 1 + transfer_queue_count;
        shared.pQueuePriorities = transfer_priorities;
        queue_infos.push_back(transfer_queue_count != 0 ? shared : queue_info);
    }
    else
    {
        queue_infos.push_back(queue_info);
        if (transfer_queue_count != 0)
            queue_infos.push_back(VkDeviceQueueCreateInfo{
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = transfer_family,
                .queueCount = transfer_queue_count,
                .pQueuePriorities = transfer_priorities,
            });
    }

    auto const device_info = VkDeviceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &vk12_features,
        .queueCreateInfoCount = u32(queue_infos.size()),
        .pQueueCreateInfos = queue_infos.data(),
        .enabledExtensionCount = u32(device_extensions.size()),
        .ppEnabledExtensionNames = device_extensions.data(),
    };

    if (VkResult r = vkCreateDevice(best_device, &device_info, nullptr, &device); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateDevice failed");

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, best_family, 0, &queue);

    // The transfer queues, if any.
    // A shared family hands them out after the graphics queue's own index.
    u32 const transfer_base_index = transfer_shares_graphics_family ? 1u : 0u;
    VkQueue transfer_queues[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    for (u32 i = 0; i < transfer_queue_count; ++i)
        vkGetDeviceQueue(device, transfer_family, transfer_base_index + i, &transfer_queues[i]);

    // Two timeline semaphores on the one queue: the epoch timeline drives reclamation, the submission timeline answers per-list completion.
    // Each starts at first-1, so nothing reads as complete before the first signal.
    if (VkResult r = create_timeline_semaphore(device, u64(sg::epoch::first) - 1, epoch_timeline); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateSemaphore (epoch timeline) failed");

    if (VkResult r = create_timeline_semaphore(device, u64(sg::submission_token::first) - 1, submission_timeline);
        r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateSemaphore (submission timeline) failed");

    auto ctx = std::make_shared<vulkan_context>(instance, best_device, device, queue, best_family, epoch_timeline,
                                                submission_timeline, messenger);

    // The context owns every handle above from here on, and its destructor releases them.
    // Disarming the guard now rather than at the end of the function is what keeps a later failure from freeing
    // them twice — once through ~vulkan_context and once through the guard.
    owned_by_context = true;

    // Descriptor sizes and the offset alignment are device properties, so the bind path reads them once here rather
    // than querying per allocation.
    auto descriptor_props = VkPhysicalDeviceDescriptorBufferPropertiesEXT{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT,
    };
    // The ray-tracing limits ride the same query, chained only where the extensions are there: asking for them on a
    // device without the extensions is undefined rather than merely empty.
    auto accel_props = VkPhysicalDeviceAccelerationStructurePropertiesKHR{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
        .pNext = &descriptor_props,
    };
    auto rt_pipeline_props = VkPhysicalDeviceRayTracingPipelinePropertiesKHR{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR,
        .pNext = &accel_props,
    };
    auto device_props = VkPhysicalDeviceProperties2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = raytracing_supported ? static_cast<void*>(&rt_pipeline_props) : static_cast<void*>(&descriptor_props),
    };
    vkGetPhysicalDeviceProperties2(best_device, &device_props);
    ctx->set_descriptor_buffer_properties(descriptor_props);
    ctx->set_device_properties(device_props.properties);

    ctx->set_adapter_info(describe_adapter(best_device));

    // Ray tracing is supported only if its entry points are actually there too, which is the same all-or-nothing rule
    // the descriptor-buffer loader applies — except that here a false answer is a legitimate device rather than a
    // reason to refuse one.
    if (raytracing_supported)
    {
        ctx->set_raytracing_properties(accel_props, rt_pipeline_props);
        raytracing_supported = ctx->_raytracing_functions.load(device);
    }
    ctx->set_raytracing_supported(raytracing_supported);
    ctx->_group_pool.initialize(*ctx);
    // Required at the floor, so device creation would have failed without it.
    ctx->set_host_query_reset(true);
    ctx->_query_system.initialize(*ctx);
    ctx->set_presentation_support(swapchain_supported, has_headless_surface);
    for (int i = 0; i < sg::window_platform_count; ++i)
        ctx->set_window_platform_supported(sg::window_platform(i), platform_surface[i]);

    // Both directions get their own queue where the family could give two, and share one otherwise.
    // A device with no spare queue at all falls back to the graphics queue, which keeps async transfer correct while
    // making it asynchronous only in the CPU sense.
    ctx->set_transfer_queues(transfer_queues[0] != VK_NULL_HANDLE ? transfer_family : best_family,
                             transfer_queues[0] != VK_NULL_HANDLE ? transfer_queues[0] : queue,
                             transfer_queues[1] != VK_NULL_HANDLE
                                 ? transfer_queues[1]
                                 : (transfer_queues[0] != VK_NULL_HANDLE ? transfer_queues[0] : queue));

    // The staging ring is part of a usable context rather than something acquired lazily: without it cmd.upload has
    // nowhere to write, so a context that cannot allocate one is not worth handing back.
    if (auto ring = ctx->_upload_inline.initialize(*ctx, config.upload_ring_bytes); ring.has_error())
        return cc::error(cc::move(ring).error());
    if (auto ring = ctx->_download_inline.initialize(*ctx, config.download_ring_bytes); ring.has_error())
        return cc::error(cc::move(ring).error());

    // The async transfer system is part of a usable context for the same reason the inline rings are: ctx.upload has
    // nowhere to stage without it.
    if (auto async = ctx->_upload_async.initialize(*ctx, config.async_upload_window_bytes); async.has_error())
        return cc::error(cc::move(async).error());
    if (auto async = ctx->_download_async.initialize(*ctx, config.async_download_window_bytes); async.has_error())
        return cc::error(cc::move(async).error());

    // The extension was required above, so a missing entry point here is a driver that advertises it without
    // implementing it — worth failing on rather than discovering at the first descriptor write.
    if (!ctx->_descriptor_functions.load(device))
        return cc::error("the device advertises VK_EXT_descriptor_buffer but does not export its entry points");

    if (auto heap
        = ctx->_descriptor_heap.initialize(*ctx, config.descriptor_heap_bytes, config.descriptor_transient_fraction);
        heap.has_error())
        return cc::error(cc::move(heap).error());

    // Now that the context exists it can own the messenger and receive its messages.
    // Best-effort, like the layer itself: without it validation still reaches the log, just not a listener.
    if (enable_validation)
    {
        auto ctx_dbg_info = dbg_info;
        ctx_dbg_info.pUserData = ctx.get();
        auto create_fn
            = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (create_fn && create_fn(instance, &ctx_dbg_info, nullptr, &messenger) == VK_SUCCESS)
            ctx->set_debug_messenger(messenger);
    }

    return context_handle(cc::move(ctx));
}
} // namespace sg
