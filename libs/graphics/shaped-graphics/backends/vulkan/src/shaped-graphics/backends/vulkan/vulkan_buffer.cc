// vulkan_buffer: GPU buffer creation and teardown.
// The buffer type itself is header-only, so the allocating create path and the destructor live here.

#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_format.hh>

namespace sg::backend::vulkan
{

vulkan_buffer::~vulkan_buffer()
{
    // Stage the GPU handles and finalizers for deletion once the current epoch retires.
    // An empty buffer with no finalizers owns nothing GPU-side, so it needs no deferral.
    if (_buffer != VK_NULL_HANDLE || _memory != VK_NULL_HANDLE || !_finalizers.empty())
    {
        vulkan_expiring_resource expiring;
        expiring.buffer = _buffer;
        expiring.memory = _memory;
        expiring.finalizers = cc::move(_finalizers);
        _ctx.schedule_deferred_deletion(cc::move(expiring));
    }
}

cc::result<vulkan_buffer_handle> vulkan_context::create_vulkan_buffer(isize size_in_bytes,
                                                                      sg::buffer_usages usage,
                                                                      sg::allocation_info const& alloc)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");
    // TEMPORARY: only dedicated allocations are implemented.
    // Placement into a memory_heap needs vkBindBufferMemory at an offset into the heap's VkDeviceMemory, which is not wired up.
    CC_ASSERT(alloc.is_dedicated(), "placed allocations (non-null memory_heap) not implemented yet");

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    // Empty buffer: no allocation (Vulkan rejects a zero-size buffer); null handles are the representation.
    if (size_in_bytes > 0)
    {
        auto const buffer_info = VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = VkDeviceSize(size_in_bytes),
            .usage = to_vk_buffer_usage(usage),
            // TODO: the streaming usages need VK_SHARING_MODE_CONCURRENT over the graphics + transfer
            // families — EXCLUSIVE cannot express two families holding a resource at once, and ownership
            // transfer serializes the very concurrency streaming exists to allow.
            // Vulkan is the strict backend here: dx12 needs a flag only for a region inside a subresource.
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };

        if (VkResult r = vkCreateBuffer(_device, &buffer_info, nullptr, &buffer); r != VK_SUCCESS)
            return vulkan_error(r, "vkCreateBuffer failed");

        VkMemoryRequirements req = {};
        vkGetBufferMemoryRequirements(_device, buffer, &req);

        u32 const type = find_memory_type(u32(req.memoryTypeBits), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX)
        {
            vkDestroyBuffer(_device, buffer, nullptr);
            return cc::error("no device-local memory type for buffer");
        }

        auto const alloc_info = VkMemoryAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex = type,
        };
        if (VkResult r = vkAllocateMemory(_device, &alloc_info, nullptr, &memory); r != VK_SUCCESS)
        {
            vkDestroyBuffer(_device, buffer, nullptr);
            return vulkan_error(r, "vkAllocateMemory failed");
        }

        if (VkResult r = vkBindBufferMemory(_device, buffer, memory, 0); r != VK_SUCCESS)
        {
            vkFreeMemory(_device, memory, nullptr);
            vkDestroyBuffer(_device, buffer, nullptr);
            return vulkan_error(r, "vkBindBufferMemory failed");
        }
    }

    return vulkan_buffer_handle(
        std::make_shared<vulkan_buffer>(*this, current_epoch(), size_in_bytes, usage, buffer, memory));
}
} // namespace sg::backend::vulkan
