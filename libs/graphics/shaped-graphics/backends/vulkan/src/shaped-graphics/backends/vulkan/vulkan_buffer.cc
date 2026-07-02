// vulkan_buffer: GPU buffer creation and teardown. The buffer type is header-only (ctor + fields);
// the allocating create path and the destructor live here.

#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

namespace sg::backend::vulkan
{
namespace
{
VkBufferUsageFlags to_vk_buffer_usage(sg::buffer_usage usage)
{
    VkBufferUsageFlags flags = 0;
    if (sg::has_flag(usage, sg::buffer_usage::copy_src))
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (sg::has_flag(usage, sg::buffer_usage::copy_dst))
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (sg::has_flag(usage, sg::buffer_usage::vertex))
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (sg::has_flag(usage, sg::buffer_usage::index))
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (sg::has_flag(usage, sg::buffer_usage::uniform))
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (sg::has_flag(usage, sg::buffer_usage::storage))
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    // Vulkan rejects a zero-usage buffer; a usage-less non-empty buffer keeps a benign transfer dst
    // so the handle stays valid (dx12's FLAG_NONE path has no such restriction).
    if (flags == 0)
        flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return flags;
}
} // namespace

vulkan_buffer::~vulkan_buffer()
{
    // Stage the GPU handles + finalizers for deletion once the current epoch retires. Empty buffers
    // (null handles) with no finalizers own nothing GPU-side and need no deferral.
    if (_buffer != VK_NULL_HANDLE || _memory != VK_NULL_HANDLE || !_finalizers.empty())
    {
        vulkan_expiring_resource expiring;
        expiring.buffer = _buffer;
        expiring.memory = _memory;
        expiring.finalizers = cc::move(_finalizers);
        _ctx.schedule_deferred_deletion(cc::move(expiring));
    }
}

cc::result<vulkan_buffer_handle> vulkan_context::create_vulkan_buffer(cc::isize size_in_bytes, sg::buffer_usage usage)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    // Empty buffer: no allocation (Vulkan rejects a zero-size buffer); null handles are the representation.
    if (size_in_bytes > 0)
    {
        auto const buffer_info = VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = VkDeviceSize(size_in_bytes),
            .usage = to_vk_buffer_usage(usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };

        if (VkResult r = vkCreateBuffer(_device, &buffer_info, nullptr, &buffer); r != VK_SUCCESS)
            return vulkan_error(r, "vkCreateBuffer failed");

        VkMemoryRequirements req = {};
        vkGetBufferMemoryRequirements(_device, buffer, &req);

        // GPU-resident: sg exposes no host-visible buffers.
        cc::u32 const type = find_memory_type(cc::u32(req.memoryTypeBits), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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

    return std::make_shared<vulkan_buffer>(*this, current_epoch(), size_in_bytes, usage, buffer, memory);
}
} // namespace sg::backend::vulkan
