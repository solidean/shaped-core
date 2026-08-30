// vulkan_context: device-level lifetime bodies (shutdown / teardown) plus small shared helpers.
// Bring-up lives in vulkan_context.create.cc, the epoch bodies in vulkan_epoch.cc.

#include <clean-core/record/domain.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

namespace sg::backend::vulkan
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "sg.vulkan");

char const* vk_result_name(VkResult r)
{
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

u32 vulkan_context::find_memory_type(u32 type_bits, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties mem = {};
    vkGetPhysicalDeviceMemoryProperties(_physical_device, &mem);
    for (u32 i = 0; i < mem.memoryTypeCount; ++i)
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

    // Release per-context routine instances first: they may cache epoch/allocator-managed resources
    // (e.g. an init_once buffer) that must be freed before the resource systems below are torn down.
    routines.clear();

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

cc::result<sg::gpu_memory_usage> sg::backend::vulkan::vulkan_context::query_gpu_memory() const
{
    if (_physical_device == VK_NULL_HANDLE)
        return cc::error("no physical device");

    // VK_EXT_memory_budget is what turns the heap sizes into a live budget.
    // Without it vulkan reports only how big the heaps ARE, which says nothing about what is left.
    // So this refuses rather than reporting a static number as a reading.
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

    VkPhysicalDeviceMemoryProperties2 properties = {};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    properties.pNext = &budget;

    vkGetPhysicalDeviceMemoryProperties2(_physical_device, &properties);

    auto out = sg::gpu_memory_usage();
    auto any = false;
    for (u32 i = 0; i < properties.memoryProperties.memoryHeapCount; ++i)
    {
        if ((properties.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
            continue;

        // A driver without the extension leaves these zero, which is how the refusal below is detected: a real device
        // with a real budget never reports zero for every device-local heap.
        out.budget_bytes += i64(budget.heapBudget[i]);
        out.current_usage_bytes += i64(budget.heapUsage[i]);
        any = true;
    }

    if (!any || out.budget_bytes == 0)
        return cc::error("VK_EXT_memory_budget is unavailable, so this device reports no memory budget");

    return out;
}
