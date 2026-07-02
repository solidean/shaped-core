// vulkan context bring-up: optional validation, instance, physical-device selection, logical device +
// graphics queue. Split off from the other vulkan_context bodies because it is the heavy path and
// grows with every device feature we opt into (queue selection, extensions, feature probing, ...).

#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/print.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

#include <cstring> // strcmp

namespace sg::backend::vulkan
{
namespace
{
char const* const k_validation_layer = "VK_LAYER_KHRONOS_validation";

// Validation messages routed to stderr. Registered on the instance when validation is active; runs on
// whatever thread the loader raises the message from. Always returns VK_FALSE — never aborts the call.
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
    cc::eprintln("[vulkan {}] {}", level, data->pMessage);
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
    auto layers = cc::vector<VkLayerProperties>::create_defaulted(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (auto const& l : layers)
        if (std::strcmp(l.layerName, k_validation_layer) == 0)
            return true;
    return false;
}

bool debug_utils_extension_available()
{
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    auto exts = cc::vector<VkExtensionProperties>::create_defaulted(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    for (auto const& e : exts)
        if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
            return true;
    return false;
}

// First queue family with graphics support. Returns false if the device has none.
bool find_graphics_queue_family(VkPhysicalDevice dev, cc::u32& out_index)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    auto families = cc::vector<VkQueueFamilyProperties>::create_defaulted(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            out_index = i;
            return true;
        }
    return false;
}

// Higher is preferred. `prefer_software` lifts CPU devices (lavapipe) to the top — the WARP analog;
// otherwise a discrete GPU wins and CPU comes last. Every mode still ranks each type so a lone
// less-ideal device is picked over nothing.
int device_type_rank(VkPhysicalDeviceType type, bool prefer_software)
{
    // Selection tiers; higher wins. Each mode maps a device type to its ideal / fallback tier.
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
    cc::u32 queue_family;
};

// Highest-ranked physical device that exposes a graphics queue, or nullopt if none qualifies (no
// devices at all, or none with a graphics queue).
cc::optional<selected_physical_device> pick_physical_device(VkInstance instance, bool prefer_software)
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    auto devices = cc::vector<VkPhysicalDevice>::create_defaulted(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    cc::optional<selected_physical_device> best;
    int best_rank = -1;
    for (auto device : devices)
    {
        cc::u32 family = 0;
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

void destroy_debug_messenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger)
{
    if (messenger == VK_NULL_HANDLE)
        return;
    auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (fn)
        fn(instance, messenger, nullptr);
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
        .apiVersion = VK_API_VERSION_1_2, // baseline we require; widely supported by current drivers
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
        .enabledLayerCount = cc::u32(layers.size()),
        .ppEnabledLayerNames = layers.data(),
        .enabledExtensionCount = cc::u32(extensions.size()),
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

    // Logical device with a single graphics queue.
    float const queue_priority = 1.0f;
    auto const queue_info = VkDeviceQueueCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = best_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };

    auto const device_info = VkDeviceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
    };

    VkDevice device = VK_NULL_HANDLE;
    if (VkResult r = vkCreateDevice(best_device, &device_info, nullptr, &device); r != VK_SUCCESS)
    {
        destroy_debug_messenger(instance, messenger);
        vkDestroyInstance(instance, nullptr);
        return vulkan_error(r, "vkCreateDevice failed");
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, best_family, 0, &queue);

    return context_handle(std::make_shared<vulkan_context>(instance, best_device, device, queue, best_family, messenger));
}
} // namespace sg
