#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_software_pool.hh>

namespace sg::backend::vulkan
{
vulkan_software_pool::vulkan_software_pool(vulkan_context& ctx)
  : sg::impl::software_acceleration_pool(ctx), _vulkan(ctx)
{
}

cc::result<sg::impl::software_pool_storage> vulkan_software_pool::create_storage(isize size_in_bytes)
{
    auto const device = _vulkan._device;
    auto out = memory();
    auto const info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(size_in_bytes),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (VkResult const r = vkCreateBuffer(device, &info, nullptr, &out.buffer); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateBuffer (software acceleration pool) failed");

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
    // Host-visible because a build writes it at record time; coherent so no flush is needed before a shader reads.
    auto const type = _vulkan.find_memory_type(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX)
    {
        destroy(out);
        return cc::error("no host-visible coherent memory type for the software acceleration pool");
    }
    auto const flags = VkMemoryAllocateFlagsInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    auto const alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    if (VkResult const r = vkAllocateMemory(device, &alloc, nullptr, &out.device_memory); r != VK_SUCCESS)
    {
        destroy(out);
        return vulkan_error(r, "vkAllocateMemory (software acceleration pool) failed");
    }
    if (VkResult const r = vkBindBufferMemory(device, out.buffer, out.device_memory, 0); r != VK_SUCCESS)
    {
        destroy(out);
        return vulkan_error(r, "vkBindBufferMemory (software acceleration pool) failed");
    }
    void* mapped = nullptr;
    if (VkResult const r = vkMapMemory(device, out.device_memory, 0, VK_WHOLE_SIZE, 0, &mapped); r != VK_SUCCESS)
    {
        destroy(out);
        return vulkan_error(r, "vkMapMemory (software acceleration pool) failed");
    }
    cc::memset(mapped, 0, size_t(size_in_bytes));

    auto const address_info = VkBufferDeviceAddressInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = out.buffer,
    };
    _previous = _current;
    _current = out;
    return sg::impl::software_pool_storage{
        .mapped = static_cast<byte*>(mapped),
        .gpu_address = u64(vkGetBufferDeviceAddress(device, &address_info)),
        .size_in_bytes = size_in_bytes,
    };
}

void vulkan_software_pool::destroy(memory m)
{
    if (m.buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(_vulkan._device, m.buffer, nullptr);
    if (m.device_memory != VK_NULL_HANDLE)
        vkFreeMemory(_vulkan._device, m.device_memory, nullptr);
}

void vulkan_software_pool::retire_storage(sg::impl::software_pool_storage)
{
    // the base retires the storage create_storage replaced, right after copying it
    _vulkan.defer_until_retired([this, old = _previous] { destroy(old); });
    _previous = {};
}

void vulkan_software_pool::shutdown()
{
    destroy(_current);
    _current = {};
}
} // namespace sg::backend::vulkan
