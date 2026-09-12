#include <clean-core/common/assert.hh>
#include <clean-core/record/log.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_upload_inline.hh>

namespace sg::backend::vulkan
{
cc::optional<vulkan_upload_inline_system::ring_storage> vulkan_upload_inline_system::create_ring(isize capacity)
{
    auto const info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(capacity),
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    auto out = ring_storage{};
    if (vkCreateBuffer(_ctx->_device, &info, nullptr, &out.buffer) != VK_SUCCESS)
        return {};

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(_ctx->_device, out.buffer, &requirements);

    // HOST_VISIBLE so the CPU can write it, HOST_COHERENT so it needs no explicit flush before the GPU reads it.
    // Coherent is the simplification worth taking here: the alternative is vkFlushMappedMemoryRanges per copy, and
    // the ranges would have to be rounded to nonCoherentAtomSize, which the seam-skipping in reserve complicates.
    u32 const type = _ctx->find_memory_type(requirements.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX)
    {
        vkDestroyBuffer(_ctx->_device, out.buffer, nullptr);
        return {};
    }

    auto const alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    if (vkAllocateMemory(_ctx->_device, &alloc, nullptr, &out.memory) != VK_SUCCESS)
    {
        vkDestroyBuffer(_ctx->_device, out.buffer, nullptr);
        return {};
    }

    if (vkBindBufferMemory(_ctx->_device, out.buffer, out.memory, 0) != VK_SUCCESS)
    {
        vkFreeMemory(_ctx->_device, out.memory, nullptr);
        vkDestroyBuffer(_ctx->_device, out.buffer, nullptr);
        return {};
    }

    // Mapped once for the ring's whole life: a per-reservation map/unmap would cost more than the copy it serves.
    void* mapped = nullptr;
    if (vkMapMemory(_ctx->_device, out.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
    {
        vkFreeMemory(_ctx->_device, out.memory, nullptr);
        vkDestroyBuffer(_ctx->_device, out.buffer, nullptr);
        return {};
    }
    out.mapped = static_cast<byte*>(mapped);
    return out;
}

cc::result<cc::unit> vulkan_upload_inline_system::initialize(vulkan_context& ctx, isize capacity_in_bytes)
{
    CC_ASSERT(capacity_in_bytes > 0, "the inline upload ring needs a non-zero capacity");
    _ctx = &ctx;

    auto built = create_ring(capacity_in_bytes);
    if (!built.has_value())
        return cc::error("could not allocate the inline upload ring");

    _buffer = built.value().buffer;
    _memory = built.value().memory;
    _mapped = built.value().mapped;
    _capacity = capacity_in_bytes;
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

vulkan_upload_allocation vulkan_upload_inline_system::reserve(isize size_in_bytes, isize alignment_in_bytes)
{
    CC_ASSERT(_mapped != nullptr, "the inline upload ring is not initialized");
    CC_ASSERT(size_in_bytes > 0, "an inline upload needs a non-zero size");

    // Larger than the whole ring: no budget makes this fit and no wait produces the space, so go straight to a
    // one-off allocation rather than round the loop once to discover the same thing.
    if (size_in_bytes > _capacity)
        return reserve_outside_ring(size_in_bytes);

    while (true)
    {
        auto const placed = _state.lock(
            [&](ring_state& s) -> cc::optional<isize>
            {
                // A reservation never straddles the seam, so one that would wrap starts at the next multiple of
                // the capacity instead and the tail is skipped.
                // That keeps the copy a single contiguous region.
                // Align first, then check the seam: an aligned start that would wrap restarts at the top, which is
                // itself aligned as long as the ring's capacity is.
                u64 const align = u64(alignment_in_bytes);
                u64 start = (s.next_pos + align - 1) / align * align;
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
        if (!_ctx->has_epochs_in_flight())
            return reserve_outside_ring(size_in_bytes);
        _ctx->wait_for_next_inflight_epoch();
    }
}

vulkan_upload_allocation vulkan_upload_inline_system::reserve_outside_ring(isize size_in_bytes)
{
    warn_outside_ring(size_in_bytes);

    auto const info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(size_in_bytes),
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(_ctx->_device, &info, nullptr, &buffer);
    CC_ASSERT(r == VK_SUCCESS, "vkCreateBuffer (oversized inline upload) failed");

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(_ctx->_device, buffer, &requirements);

    u32 const type = _ctx->find_memory_type(requirements.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CC_ASSERT(type != UINT32_MAX, "no host-visible coherent memory type for an oversized inline upload");

    auto const alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    r = vkAllocateMemory(_ctx->_device, &alloc, nullptr, &memory);
    CC_ASSERT(r == VK_SUCCESS, "vkAllocateMemory (oversized inline upload) failed");

    r = vkBindBufferMemory(_ctx->_device, buffer, memory, 0);
    CC_ASSERT(r == VK_SUCCESS, "vkBindBufferMemory (oversized inline upload) failed");

    void* mapped = nullptr;
    r = vkMapMemory(_ctx->_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    CC_ASSERT(r == VK_SUCCESS, "vkMapMemory (oversized inline upload) failed");

    // The same lifetime the ring span would have had: the copy reading these bytes is recorded into a list submitted
    // in the open epoch, so the epoch fence is exactly what proves the GPU is done with them.
    // vkFreeMemory unmaps implicitly, so the mapping needs no finalizer of its own.
    _ctx->schedule_deferred_deletion(vulkan_expiring_resource{.buffer = buffer, .memory = memory});

    return vulkan_upload_allocation{.buffer = buffer, .offset = 0, .mapped = static_cast<byte*>(mapped)};
}

void vulkan_upload_inline_system::warn_outside_ring(isize size_in_bytes)
{
    // Once per epoch: a frame that overruns does it for every transfer in the frame, and one line per transfer would
    // bury the one fact a caller needs.
    auto const epoch = u64(_ctx->current_epoch());
    auto seen = _last_warned_epoch.load(cc::memory_order_relaxed);
    if (seen == epoch || !_last_warned_epoch.compare_exchange_strong(seen, epoch, cc::memory_order_relaxed))
        return;

    CC_LOG_WARNING("an inline upload of {} bytes did not fit the {}-byte upload ring, so it was staged in a "
                   "one-off allocation — correct but slow. Raise ctx.upload.set_inline_budget past the peak an "
                   "epoch uploads",
                   size_in_bytes, _capacity);
}

void vulkan_upload_inline_system::set_budget(isize capacity)
{
    CC_ASSERT(capacity > 0, "upload ring capacity must be positive");
    _state.lock([&](ring_state& s) { s.pending_capacity = capacity; });
}

void vulkan_upload_inline_system::apply_pending_budget()
{
    isize const pending = _state.lock([](ring_state& s) { return s.pending_capacity; });
    if (pending <= 0)
        return;

    // Drain every in-flight epoch so no GPU work still reads the ring, then retire them to reclaim their space.
    while (u64(_ctx->completed_epoch()) + 1 < u64(_ctx->current_epoch()))
        _ctx->wait_for_next_inflight_epoch();
    _ctx->process_completed_epochs();

    // Built before the old one is released, so a failed allocation leaves the current ring intact.
    auto built = create_ring(pending);
    CC_ASSERT(built.has_value(), "inline upload ring resize failed to allocate");

    if (_memory != VK_NULL_HANDLE)
    {
        if (_mapped != nullptr)
            vkUnmapMemory(_ctx->_device, _memory);
        vkFreeMemory(_ctx->_device, _memory, nullptr);
    }
    if (_buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(_ctx->_device, _buffer, nullptr);

    _buffer = built.value().buffer;
    _memory = built.value().memory;
    _mapped = built.value().mapped;
    _capacity = pending;
    _state.lock(
        [](ring_state& s)
        {
            s.next_pos = 0;
            s.freed_pos = 0;
            s.checkpoints.clear(); // drained above; the fresh ring restarts its logical cursor at 0
            s.pending_capacity = 0;
        });
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
