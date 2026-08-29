#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_upload_inline.hh>

namespace sg::backend::vulkan
{
cc::result<cc::unit> vulkan_upload_inline_system::initialize(vulkan_context& ctx, isize capacity_in_bytes)
{
    CC_ASSERT(capacity_in_bytes > 0, "the inline upload ring needs a non-zero capacity");
    _ctx = &ctx;
    _capacity = capacity_in_bytes;

    auto const info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(capacity_in_bytes),
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (VkResult const r = vkCreateBuffer(ctx._device, &info, nullptr, &_buffer); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateBuffer (inline upload ring) failed");

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(ctx._device, _buffer, &requirements);

    // HOST_VISIBLE so the CPU can write it, HOST_COHERENT so it needs no explicit flush before the GPU reads it.
    // Coherent is the simplification worth taking here: the alternative is vkFlushMappedMemoryRanges per copy, and
    // the ranges would have to be rounded to nonCoherentAtomSize, which the seam-skipping below already complicates.
    u32 const type = ctx.find_memory_type(requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX)
    {
        vkDestroyBuffer(ctx._device, _buffer, nullptr);
        _buffer = VK_NULL_HANDLE;
        return cc::error("no host-visible coherent memory type for the inline upload ring");
    }

    auto const alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    if (VkResult const r = vkAllocateMemory(ctx._device, &alloc, nullptr, &_memory); r != VK_SUCCESS)
    {
        vkDestroyBuffer(ctx._device, _buffer, nullptr);
        _buffer = VK_NULL_HANDLE;
        return vulkan_error(r, "vkAllocateMemory (inline upload ring) failed");
    }

    if (VkResult const r = vkBindBufferMemory(ctx._device, _buffer, _memory, 0); r != VK_SUCCESS)
    {
        shutdown();
        return vulkan_error(r, "vkBindBufferMemory (inline upload ring) failed");
    }

    // Mapped once for the ring's whole life: a per-reservation map/unmap would cost more than the copy it serves.
    void* mapped = nullptr;
    if (VkResult const r = vkMapMemory(ctx._device, _memory, 0, VK_WHOLE_SIZE, 0, &mapped); r != VK_SUCCESS)
    {
        shutdown();
        return vulkan_error(r, "vkMapMemory (inline upload ring) failed");
    }
    _mapped = static_cast<byte*>(mapped);
    return cc::unit{};
}

void vulkan_upload_inline_system::shutdown()
{
    if (_ctx == nullptr)
        return;

    if (_memory != VK_NULL_HANDLE)
    {
        if (_mapped != nullptr)
            vkUnmapMemory(_ctx->_device, _memory);
        vkFreeMemory(_ctx->_device, _memory, nullptr);
    }
    if (_buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(_ctx->_device, _buffer, nullptr);

    _mapped = nullptr;
    _memory = VK_NULL_HANDLE;
    _buffer = VK_NULL_HANDLE;
    _capacity = 0;
    _ctx = nullptr;
}

vulkan_upload_allocation vulkan_upload_inline_system::reserve(isize size_in_bytes)
{
    CC_ASSERT(_mapped != nullptr, "the inline upload ring is not initialized");
    CC_ASSERT(size_in_bytes > 0 && size_in_bytes <= _capacity, "an inline upload larger than the whole ring cannot be "
                                                               "staged");

    while (true)
    {
        auto const placed = _state.lock(
            [&](ring_state& s) -> cc::optional<isize>
            {
                // A reservation never straddles the seam, so one that would wrap starts at the next multiple of
                // the capacity instead and the tail is skipped.
                // That keeps the copy a single contiguous region.
                u64 start = s.next_pos;
                isize const offset = isize(start % u64(_capacity));
                if (offset + size_in_bytes > _capacity)
                    start += u64(_capacity - offset);

                // In flight is everything between freed_pos and the end of this reservation.
                if (start + u64(size_in_bytes) - s.freed_pos > u64(_capacity))
                    return {};

                s.next_pos = start + u64(size_in_bytes);
                return isize(start % u64(_capacity));
            });

        if (placed.has_value())
            return vulkan_upload_allocation{.buffer = _buffer,
                                            .offset = placed.value(),
                                            .mapped = _mapped + placed.value()};

        // Full.
        // Waiting only helps if an epoch is still in flight to reclaim; otherwise this one epoch's uploads genuinely
        // exceed the ring, and no amount of waiting changes that.
        CC_ASSERT(_ctx->has_epochs_in_flight(), "inline uploads in one epoch exceed the upload ring capacity — raise "
                                                "ctx.upload.set_inline_budget");
        _ctx->wait_for_next_inflight_epoch();
    }
}

void vulkan_upload_inline_system::on_epoch_advance(sg::epoch closed)
{
    if (_mapped == nullptr)
        return;
    _state.lock([&](ring_state& s) { s.checkpoints.push_back(checkpoint{.epoch_id = closed, .end_pos = s.next_pos}); });
}

void vulkan_upload_inline_system::on_epochs_completed(sg::epoch completed)
{
    if (_mapped == nullptr)
        return;
    _state.lock(
        [&](ring_state& s)
        {
            // Checkpoints are monotonic in both epoch and end_pos, so the leading finished run is a prefix.
            isize kept = 0;
            for (auto const& cp : s.checkpoints)
            {
                if (u64(cp.epoch_id) > u64(completed))
                    break;
                s.freed_pos = cp.end_pos;
                ++kept;
            }
            if (kept > 0)
                s.checkpoints.remove_at_range({.offset = 0, .size = kept});
        });
}
} // namespace sg::backend::vulkan
