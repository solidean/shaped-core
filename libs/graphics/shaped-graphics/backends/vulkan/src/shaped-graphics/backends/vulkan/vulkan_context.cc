// vulkan_context: device-level lifetime bodies (shutdown / teardown) plus small shared helpers. The
// heavier bring-up path lives in vulkan_context.create.cc.

#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

namespace sg::backend::vulkan
{
char const* vk_result_name(VkResult r)
{
    // Core results up to Vulkan 1.2 (our declared baseline). Extension / newer codes fall through to
    // the default; vulkan_error still prints the numeric value.
    switch (r)
    {
    // 1.0 success codes
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    // 1.0 error codes
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN:
        return "VK_ERROR_UNKNOWN";
    // promoted in 1.1
    case VK_ERROR_OUT_OF_POOL_MEMORY:
        return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
        return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    // promoted in 1.2
    case VK_ERROR_FRAGMENTATION:
        return "VK_ERROR_FRAGMENTATION";
    case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
        return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    default:
        return "VK_RESULT_<unknown>";
    }
}

cc::u32 vulkan_context::find_memory_type(cc::u32 type_bits, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties mem = {};
    vkGetPhysicalDeviceMemoryProperties(_physical_device, &mem);
    for (cc::u32 i = 0; i < mem.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    return UINT32_MAX;
}

bool vulkan_context::note_device_lost_if_lost(VkResult r, char const* what)
{
    if (r != VK_ERROR_DEVICE_LOST)
        return false;
    mark_device_lost(cc::format("{} ({})", what, vk_result_name(r)));
    return true;
}

void vulkan_context::shutdown()
{
    if (_is_shut_down)
        return;

    // Advance-and-wait-for-idle drains the GPU, then closes and retires the final epoch — freeing every
    // resource (in-flight and staged) and running finalizers — before the device is released.
    // Externally synchronized: no create/submit/drop may run concurrently with shutdown.
    if (_device != VK_NULL_HANDLE && _epoch_timeline != VK_NULL_HANDLE)
        advance_epoch_and_wait_for_idle();

    if (_device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(_device);

        // Every command pool is idle now (the drain retired every in-flight epoch, returning pools to
        // the free set); destroy them before the device.
        _command_pools.lock(
            [&](vulkan_command_pool_set& p)
            {
                for (auto& cp : p.free)
                    vkDestroyCommandPool(_device, cp.pool, nullptr);
                p.free = {};
                p.in_epoch = {};
            });

        if (_submission_timeline != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(_device, _submission_timeline, nullptr);
            _submission_timeline = VK_NULL_HANDLE;
        }
        if (_epoch_timeline != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(_device, _epoch_timeline, nullptr);
            _epoch_timeline = VK_NULL_HANDLE;
        }

        vkDestroyDevice(_device, nullptr); // _queue is owned by the device
        _device = VK_NULL_HANDLE;
    }

    if (_debug_messenger != VK_NULL_HANDLE)
    {
        auto fn
            = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (fn)
            fn(_instance, _debug_messenger, nullptr);
        _debug_messenger = VK_NULL_HANDLE;
    }

    if (_instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(_instance, nullptr); // _physical_device is owned by the instance
        _instance = VK_NULL_HANDLE;
    }

    _is_shut_down = true;
}
} // namespace sg::backend::vulkan
