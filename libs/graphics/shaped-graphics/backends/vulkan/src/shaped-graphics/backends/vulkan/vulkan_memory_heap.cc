#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_format.hh>
#include <shaped-graphics/backends/vulkan/vulkan_memory_heap.hh>

namespace sg::backend::vulkan
{
cc::result<vulkan_memory_heap_handle> vulkan_memory_heap::create(vulkan_context& ctx, isize size_in_bytes)
{
    CC_ASSERT(size_in_bytes >= 0, "heap size must be non-negative");
    if (size_in_bytes == 0)
        return vulkan_memory_heap_handle(std::make_shared<vulkan_memory_heap>(ctx, VK_NULL_HANDLE, 0));

    // The memory type has to serve every buffer that will be placed here, and the caller has not said which those
    // are yet.
    // A probe carrying every usage sg can place therefore gives the conservative type mask: memoryTypeBits only ever
    // narrows as usages are added, so a type serving all of them serves any subset.
    constexpr auto every_usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst
                               | sg::buffer_usage::vertex_buffer | sg::buffer_usage::index_buffer
                               | sg::buffer_usage::uniform_buffer | sg::buffer_usage::readonly_buffer
                               | sg::buffer_usage::readwrite_buffer | sg::buffer_usage::indirect_command_buffer;
    auto const probe_info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(size_in_bytes),
        .usage = to_vk_buffer_usage(every_usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer probe = VK_NULL_HANDLE;
    if (VkResult const r = vkCreateBuffer(ctx._device, &probe_info, nullptr, &probe); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateBuffer (memory heap probe) failed");

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(ctx._device, probe, &requirements);
    vkDestroyBuffer(ctx._device, probe, nullptr);

    u32 const type = ctx.find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
        return cc::error("no device-local memory type for a memory heap");

    // Buffers placed here take their device address like any other, so the backing allocation needs the flag too.
    auto const alloc_flags = VkMemoryAllocateFlagsInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    auto const alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &alloc_flags,
        .allocationSize = VkDeviceSize(size_in_bytes),
        .memoryTypeIndex = type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (VkResult const r = vkAllocateMemory(ctx._device, &alloc, nullptr, &memory); r != VK_SUCCESS)
        return vulkan_error(r, "vkAllocateMemory (memory heap) failed");

    return vulkan_memory_heap_handle(std::make_shared<vulkan_memory_heap>(ctx, memory, size_in_bytes));
}

vulkan_memory_heap::~vulkan_memory_heap()
{
    if (_memory != VK_NULL_HANDLE)
        vkFreeMemory(_ctx._device, _memory, nullptr);
}

sg::memory_requirements vulkan_memory_heap::query_buffer_requirements(isize size_in_bytes, sg::buffer_usages usage) const
{
    if (size_in_bytes == 0)
        return sg::memory_requirements{.alignment_in_bytes = 1, .size_in_bytes = 0};

    auto const info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(size_in_bytes),
        .usage = to_vk_buffer_usage(usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer probe = VK_NULL_HANDLE;
    VkResult const r = vkCreateBuffer(_ctx._device, &info, nullptr, &probe);
    CC_ASSERT(r == VK_SUCCESS, "vkCreateBuffer (requirements probe) failed");

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(_ctx._device, probe, &requirements);
    vkDestroyBuffer(_ctx._device, probe, nullptr);

    return sg::memory_requirements{
        .alignment_in_bytes = isize(requirements.alignment),
        .size_in_bytes = isize(requirements.size),
    };
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
cc::result<vulkan_memory_heap_handle> vulkan_context::create_vulkan_memory_heap(isize size_in_bytes)
{
    return vulkan_memory_heap::create(*this, size_in_bytes);
}
} // namespace sg::backend::vulkan
