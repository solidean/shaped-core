#include <clean-core/common/assert.hh>
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

cc::result<cc::unit> vulkan_download_inline_system::initialize(vulkan_context& ctx, isize capacity_in_bytes)
{
    CC_ASSERT(capacity_in_bytes > 0, "the inline readback ring needs a non-zero capacity");
    _ctx = &ctx;
    _capacity = capacity_in_bytes;

    auto const info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(capacity_in_bytes),
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (VkResult const r = vkCreateBuffer(ctx._device, &info, nullptr, &_buffer); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateBuffer (inline readback ring) failed");

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(ctx._device, _buffer, &requirements);

    // Coherent, so reading needs no vkInvalidateMappedMemoryRanges.
    // HOST_CACHED would read faster, but it is not guaranteed coherent, and the invalidate would have to be rounded
    // to nonCoherentAtomSize, which a ring hands out unaligned offsets from.
    // Worth revisiting when readback shows up in a profile rather than before.
    u32 const type = ctx.find_memory_type(requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX)
    {
        vkDestroyBuffer(ctx._device, _buffer, nullptr);
        _buffer = VK_NULL_HANDLE;
        return cc::error("no host-visible coherent memory type for the inline readback ring");
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
        return vulkan_error(r, "vkAllocateMemory (inline readback ring) failed");
    }

    if (VkResult const r = vkBindBufferMemory(ctx._device, _buffer, _memory, 0); r != VK_SUCCESS)
    {
        shutdown();
        return vulkan_error(r, "vkBindBufferMemory (inline readback ring) failed");
    }

    void* mapped = nullptr;
    if (VkResult const r = vkMapMemory(ctx._device, _memory, 0, VK_WHOLE_SIZE, 0, &mapped); r != VK_SUCCESS)
    {
        shutdown();
        return vulkan_error(r, "vkMapMemory (inline readback ring) failed");
    }
    _mapped = static_cast<byte const*>(mapped);

    _actor = cc::make_and_start_threaded_actor<vulkan_download_actor>(*this);
    return cc::unit{};
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
    CC_ASSERT(size_in_bytes > 0 && size_in_bytes <= _capacity, "an inline readback larger than the whole ring cannot "
                                                               "be staged");

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

        CC_ASSERT(_ctx->has_epochs_in_flight(), "inline readbacks in one epoch exceed the readback ring capacity — "
                                                "raise ctx.download.set_budget");
        _ctx->wait_for_next_inflight_epoch();
    }
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
