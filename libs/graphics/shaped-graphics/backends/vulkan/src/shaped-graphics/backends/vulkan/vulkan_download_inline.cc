#include <clean-core/common/assert.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/thread_pump.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_download_inline.hh>

namespace sg::backend::vulkan
{
void vulkan_download_actor::on_message(vulkan_download_copy_job job)
{
    // Yield before blocking.
    // Where this actor has no thread of its own it runs on whoever swept the pump registry, so blocking here would
    // stall every other cooperative worker along with it — including whatever the submission is itself waiting on.
    // That is a deadlock rather than a slow path, which is why the wait below is only reached once pumping is done.
    while (!_system.submission_complete(job.token) && cc::thread_pump_all())
    {
    }
    _system.wait_for_submission(job.token);

    // A destination dropped mid-flight is a cancellation, not a delivery: the bytes were never written anywhere the
    // caller can see, so anything holding completion() must see that rather than a success it cannot act on.
    bool const wanted = job.pin.lock() != nullptr;
    if (wanted && job.deferred_cpu_copy)
        job.deferred_cpu_copy();

    if (job.completion)
    {
        if (wanted)
            job.completion->push_value(cc::unit{});
        else
            job.completion->push_error(cc::async_error::make_cancelled());
    }

    _system.on_copy_done(job.epoch_copies);
}

cc::optional<vulkan_download_inline_system::ring_storage> vulkan_download_inline_system::create_ring(isize capacity)
{
    auto const info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(capacity),
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(_ctx->_device, &info, nullptr, &buffer) != VK_SUCCESS)
        return {};

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(_ctx->_device, buffer, &requirements);

    // Coherent, so reading needs no vkInvalidateMappedMemoryRanges.
    // HOST_CACHED would read faster, but it is not guaranteed coherent, and the invalidate would have to be rounded
    // to nonCoherentAtomSize, which a ring hands out unaligned offsets from.
    // Worth revisiting when readback shows up in a profile rather than before.
    u32 const type = _ctx->find_memory_type(requirements.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX)
    {
        vkDestroyBuffer(_ctx->_device, buffer, nullptr);
        return {};
    }

    auto const alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(_ctx->_device, &alloc, nullptr, &memory) != VK_SUCCESS)
    {
        vkDestroyBuffer(_ctx->_device, buffer, nullptr);
        return {};
    }

    if (vkBindBufferMemory(_ctx->_device, buffer, memory, 0) != VK_SUCCESS)
    {
        vkFreeMemory(_ctx->_device, memory, nullptr);
        vkDestroyBuffer(_ctx->_device, buffer, nullptr);
        return {};
    }

    void* mapped = nullptr;
    if (vkMapMemory(_ctx->_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
    {
        vkFreeMemory(_ctx->_device, memory, nullptr);
        vkDestroyBuffer(_ctx->_device, buffer, nullptr);
        return {};
    }
    return ring_storage{.buffer = buffer, .memory = memory, .mapped = static_cast<byte const*>(mapped)};
}

cc::result<cc::unit> vulkan_download_inline_system::initialize(vulkan_context& ctx, isize capacity_in_bytes)
{
    CC_ASSERT(capacity_in_bytes > 0, "the inline readback ring needs a non-zero capacity");
    _ctx = &ctx;

    auto built = create_ring(capacity_in_bytes);
    if (!built.has_value())
        return cc::error("could not allocate the inline readback ring");

    _buffer = built.value().buffer;
    _memory = built.value().memory;
    _mapped = built.value().mapped;
    _capacity = capacity_in_bytes;

    _actor = cc::make_and_start_threaded_actor<vulkan_download_actor>(*this);
    return cc::unit{};
}

void vulkan_download_inline_system::set_budget(isize capacity)
{
    CC_ASSERT(capacity > 0, "readback ring capacity must be positive");
    _state.lock([&](ring_state& s) { s.pending_capacity = capacity; });
}

void vulkan_download_inline_system::apply_pending_budget()
{
    isize const pending = _state.lock([](ring_state& s) { return s.pending_capacity; });
    if (pending <= 0)
        return;

    while (u64(_ctx->completed_epoch()) + 1 < u64(_ctx->current_epoch()))
        _ctx->wait_for_next_inflight_epoch();
    _ctx->process_completed_epochs();

    // And the actor, which the epoch fence says nothing about: it memcpys OUT of this ring on its own thread, so
    // freeing on the fence alone would pull the memory out from under a copy in progress.
    _drain.wait_until_idle();

    auto built = create_ring(pending);
    CC_ASSERT(built.has_value(), "inline readback ring resize failed to allocate");

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
            s.checkpoints.clear();
            s.pending_capacity = 0;
        });
}

void vulkan_download_inline_system::shutdown()
{
    if (_ctx == nullptr)
        return;

    // Drain before the ring goes: every outstanding job memcpys out of it.
    if (_actor)
    {
        _actor->shutdown();
        _actor = {};
    }

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

vulkan_download_inline_system::reservation vulkan_download_inline_system::reserve(isize size_in_bytes,
                                                                                  isize alignment_in_bytes)
{
    CC_ASSERT(_mapped != nullptr, "the inline readback ring is not initialized");
    CC_ASSERT(size_in_bytes > 0, "an inline readback needs a non-zero size");

    // Larger than the whole ring: no budget makes this fit and no wait produces the space.
    if (size_in_bytes > _capacity)
        return reserve_outside_ring(size_in_bytes);

    while (true)
    {
        auto placed = _state.lock(
            [&](ring_state& s) -> cc::optional<reservation>
            {
                reclaim(s, _last_completed);

                u64 const align = u64(alignment_in_bytes);
                u64 start = (s.next_pos + align - 1) / align * align;
                isize const offset = isize(start % u64(_capacity));
                if (offset + size_in_bytes > _capacity)
                    start += u64(_capacity - offset);

                if (start + u64(size_in_bytes) - s.freed_pos > u64(_capacity))
                    return {};

                s.next_pos = start + u64(size_in_bytes);
                isize const placed_offset = isize(start % u64(_capacity));
                return reservation{.buffer = _buffer,
                                   .offset = placed_offset,
                                   .mapped = _mapped + placed_offset,
                                   .epoch_copies = s.current_epoch_copies};
            });

        if (placed.has_value())
            return cc::move(placed).value();

        if (!_ctx->has_epochs_in_flight())
            return reserve_outside_ring(size_in_bytes);
        _ctx->wait_for_next_inflight_epoch();
    }
}

vulkan_download_inline_system::reservation vulkan_download_inline_system::reserve_outside_ring(isize size_in_bytes)
{
    warn_outside_ring(size_in_bytes);

    auto const info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(size_in_bytes),
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(_ctx->_device, &info, nullptr, &buffer);
    CC_ASSERT(r == VK_SUCCESS, "vkCreateBuffer (oversized inline readback) failed");

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(_ctx->_device, buffer, &requirements);

    u32 const type = _ctx->find_memory_type(requirements.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CC_ASSERT(type != UINT32_MAX, "no host-visible coherent memory type for an oversized inline readback");

    auto const alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    r = vkAllocateMemory(_ctx->_device, &alloc, nullptr, &memory);
    CC_ASSERT(r == VK_SUCCESS, "vkAllocateMemory (oversized inline readback) failed");

    r = vkBindBufferMemory(_ctx->_device, buffer, memory, 0);
    CC_ASSERT(r == VK_SUCCESS, "vkBindBufferMemory (oversized inline readback) failed");

    void* mapped = nullptr;
    r = vkMapMemory(_ctx->_device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    CC_ASSERT(r == VK_SUCCESS, "vkMapMemory (oversized inline readback) failed");

    // Released through the epoch machinery rather than destroyed here: the last reference is dropped by the actor, on
    // its own thread, and routing it back through deferred deletion keeps every vkDestroy on the path that already
    // orders them against the GPU.
    auto* const ctx = _ctx;
    auto keep_alive = std::shared_ptr<void>(
        static_cast<void*>(buffer), [ctx, buffer, memory](void*)
        { ctx->schedule_deferred_deletion(vulkan_expiring_resource{.buffer = buffer, .memory = memory}); });

    return reservation{.buffer = buffer,
                       .offset = 0,
                       .mapped = static_cast<byte const*>(mapped),
                       .epoch_copies = _state.lock([](ring_state& s) { return s.current_epoch_copies; }),
                       .keep_alive = cc::move(keep_alive)};
}

void vulkan_download_inline_system::warn_outside_ring(isize size_in_bytes)
{
    auto const epoch = u64(_ctx->current_epoch());
    auto seen = _last_warned_epoch.load(cc::memory_order_relaxed);
    if (seen == epoch || !_last_warned_epoch.compare_exchange_strong(seen, epoch, cc::memory_order_relaxed))
        return;

    CC_LOG_WARNING("an inline readback of {} bytes did not fit the {}-byte readback ring, so it was staged in a "
                   "one-off allocation — correct but slow. Raise ctx.download.set_budget past the peak an epoch "
                   "reads back",
                   size_in_bytes, _capacity);
}

void vulkan_download_inline_system::account_pending_copy(std::shared_ptr<std::atomic<isize>> const& epoch_copies)
{
    if (epoch_copies)
        epoch_copies->fetch_add(1, std::memory_order_relaxed);
}

void vulkan_download_inline_system::on_copy_done(std::shared_ptr<std::atomic<isize>> const& epoch_copies)
{
    if (epoch_copies)
        epoch_copies->fetch_sub(1, std::memory_order_acq_rel);
}

bool vulkan_download_inline_system::submission_complete(sg::submission_token token) const
{
    return _ctx != nullptr && _ctx->is_submission_complete(token);
}

void vulkan_download_inline_system::wait_for_submission(sg::submission_token token)
{
    if (_ctx == nullptr || token == sg::submission_token::not_submitted)
        return;
    _ctx->wait_for_submission_token(token);
}

void vulkan_download_inline_system::enqueue_submitted(sg::submission_token token,
                                                      cc::vector<vulkan_download_copy_job>& jobs)
{
    for (auto& job : jobs)
    {
        job.token = token;
        if (job.gate)
            job.gate->mark_submitted();
        job.drain = _drain.start(); // counted from HERE: before this, nothing could have driven it
        _actor->enqueue_message(cc::move(job));
    }
    jobs.clear();
}

void vulkan_download_inline_system::discard_unsubmitted(cc::vector<vulkan_download_copy_job>& jobs)
{
    for (auto& job : jobs)
    {
        if (job.completion)
            job.completion->push_error(cc::async_error::make_cancelled());
        on_copy_done(job.epoch_copies);
    }
    jobs.clear();
}

void vulkan_download_inline_system::on_epoch_advance(sg::epoch closed)
{
    if (_mapped == nullptr)
        return;
    _state.lock(
        [&](ring_state& s)
        {
            s.checkpoints.push_back(
                checkpoint{.epoch_id = closed, .end_pos = s.next_pos, .outstanding = s.current_epoch_copies});
            s.current_epoch_copies = std::make_shared<std::atomic<isize>>(0);
        });
}

void vulkan_download_inline_system::on_epochs_completed(sg::epoch completed)
{
    if (_mapped == nullptr)
        return;
    _last_completed = completed;
    _state.lock([&](ring_state& s) { reclaim(s, completed); });
}

void vulkan_download_inline_system::reclaim(ring_state& s, sg::epoch completed)
{
    // The FIFO is ordered by allocation, so a still-busy epoch blocks reclaim of everything reserved after it.
    isize freed = 0;
    for (auto const& cp : s.checkpoints)
    {
        if (u64(cp.epoch_id) > u64(completed))
            break;
        if (cp.outstanding && cp.outstanding->load(std::memory_order_acquire) != 0)
            break; // retired on the GPU, but the actor has not finished reading it yet
        s.freed_pos = cp.end_pos;
        ++freed;
    }
    if (freed > 0)
        s.checkpoints.remove_at_range({.offset = 0, .size = freed});
}
} // namespace sg::backend::vulkan
