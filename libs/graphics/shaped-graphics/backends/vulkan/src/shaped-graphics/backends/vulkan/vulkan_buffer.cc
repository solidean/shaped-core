// vulkan_buffer: GPU buffer creation and teardown.
// The buffer type itself is header-only, so the allocating create path and the destructor live here.

#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_format.hh>

namespace sg::backend::vulkan
{
VkDevice ctx_device_of(vulkan_context& ctx)
{
    return ctx._device;
}


vulkan_completion_group_handle ctx_acquire_completion_group(vulkan_context& ctx)
{
    return ctx._group_pool.acquire();
}

std::shared_ptr<vulkan_buffer> vulkan_context::register_if_transient(std::shared_ptr<vulkan_buffer> buffer,
                                                                     sg::lifetime_scope scope)
{
    if (scope == sg::lifetime_scope::transient)
        _transient_expiring.lock([&](cc::vector<std::weak_ptr<sg::raw_buffer const>>& v) { v.push_back(buffer); });
    return buffer;
}

void vulkan_buffer::release_storage() const
{
    // Stage the GPU handles and finalizers for deletion once the current epoch retires.
    // An empty buffer with no finalizers owns nothing GPU-side, so it needs no deferral.
    // Idempotent: the handles are cleared here, so expiry and destruction cannot stage the same ones twice.
    if (_buffer == VK_NULL_HANDLE && _memory == VK_NULL_HANDLE && _finalizers.empty())
        return;

    vulkan_expiring_resource expiring;
    expiring.buffer = _buffer;
    expiring.memory = _memory;
    expiring.finalizers = cc::move(_finalizers);

    // The transfer queue may still be copying into or out of this buffer, which the epoch says nothing about.
    // Both directions share one entry, so the gate takes whichever timeline actually has something pending — they
    // are different groups, and a value from one means nothing on the other.
    auto const upload_pending = _pending_async_upload_value.load(cc::memory_order_acquire);
    auto const stream_pending = _pending_stream_copy_value.load(cc::memory_order_acquire);
    auto const download_pending = _pending_async_download_value.load(cc::memory_order_acquire);
    auto const stream_download_pending = _pending_stream_download_value.load(cc::memory_order_acquire);
    auto const highest_upload = upload_pending > stream_pending ? upload_pending : stream_pending;
    auto const highest_download = download_pending > stream_download_pending ? download_pending : stream_download_pending;
    if (highest_upload != 0 && _upload_group != nullptr)
        expiring.copy_wait = {.group = _upload_group, .value = highest_upload};
    else if (highest_download != 0 && _download_group != nullptr)
        expiring.copy_wait = {.group = _download_group, .value = highest_download};
    _buffer = VK_NULL_HANDLE;
    _memory = VK_NULL_HANDLE;
    _ctx.schedule_deferred_deletion(cc::move(expiring));
}

void vulkan_buffer::on_expired() const
{
    release_storage();
}

vulkan_buffer::~vulkan_buffer()
{
    release_storage();
} // no-op if expire() already released the storage

cc::result<vulkan_buffer_handle> vulkan_context::create_vulkan_buffer(isize size_in_bytes,
                                                                      sg::buffer_usages usage,
                                                                      sg::allocation_info const& alloc,
                                                                      VkBufferUsageFlags extra_usage)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    // Empty buffer: no allocation (Vulkan rejects a zero-size buffer); null handles are the representation.
    if (size_in_bytes > 0)
    {
        // A buffer any async transfer can touch is shared CONCURRENTLY between the graphics and transfer families.
        //
        // EXCLUSIVE cannot express two families holding a resource at once, and the alternative — a queue-family
        // ownership transfer, which is a release barrier on one queue paired with an acquire on the other —
        // serializes exactly the concurrency async transfer exists to provide.
        // Vulkan is the strict backend here: D3D12 has no notion of queue ownership for a buffer at all.
        //
        // Only where the families actually differ: CONCURRENT naming one family is invalid, and a device that gave
        // no separate transfer queue has nothing to share with.
        u32 const families[2] = {_queue_family_index, _transfer_queue_family};
        bool const shared
            = has_dedicated_transfer_queue() && usage.has_any(sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst);

        auto const buffer_info = VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = VkDeviceSize(size_in_bytes),
            .usage = to_vk_buffer_usage(usage) | extra_usage,
            .sharingMode = shared ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = shared ? 2u : 0u,
            .pQueueFamilyIndices = shared ? families : nullptr,
        };

        if (VkResult r = vkCreateBuffer(_device, &buffer_info, nullptr, &buffer); r != VK_SUCCESS)
            return vulkan_error(r, "vkCreateBuffer failed");

        VkMemoryRequirements req = {};
        vkGetBufferMemoryRequirements(_device, buffer, &req);

        if (alloc.is_placed())
        {
            // Placed: the heap owns the memory and the caller picked the offset, so this binds and allocates nothing.
            // The buffer holds a handle to the heap, which is what keeps the memory alive under the placement.
            auto const heap = std::dynamic_pointer_cast<vulkan_memory_heap const>(alloc.heap);
            CC_ASSERT(heap != nullptr, "allocation heap is not a vulkan memory heap");
            CC_ASSERT(alloc.offset >= 0 && alloc.offset + isize(req.size) <= heap->size_in_bytes(),
                      "placement does not fit inside the heap");
            CC_ASSERT(alloc.offset % isize(req.alignment) == 0, "placement offset violates the buffer's alignment");

            if (VkResult r = vkBindBufferMemory(_device, buffer, heap->_memory, VkDeviceSize(alloc.offset));
                r != VK_SUCCESS)
            {
                vkDestroyBuffer(_device, buffer, nullptr);
                return vulkan_error(r, "vkBindBufferMemory (placed) failed");
            }

            return vulkan_buffer_handle(
                register_if_transient(std::make_shared<vulkan_buffer>(*this, current_epoch(), size_in_bytes, usage,
                                                                      buffer, VK_NULL_HANDLE, alloc.heap),
                                      alloc.scope));
        }

        u32 const type = find_memory_type(u32(req.memoryTypeBits), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX)
        {
            vkDestroyBuffer(_device, buffer, nullptr);
            return cc::error("no device-local memory type for buffer");
        }

        // The device-address flag is required on any allocation backing a buffer whose address is taken, which is
        // every buffer here — see to_vk_buffer_usage.
        auto const alloc_flags = VkMemoryAllocateFlagsInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        };
        auto const alloc_info = VkMemoryAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &alloc_flags,
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

    return vulkan_buffer_handle(register_if_transient(
        std::make_shared<vulkan_buffer>(*this, current_epoch(), size_in_bytes, usage, buffer, memory), alloc.scope));
}
} // namespace sg::backend::vulkan
