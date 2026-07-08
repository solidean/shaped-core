// vulkan epoch system: advance/retire, waits, and deferred-deletion staging. The epoch *concept*
// (counter + contract) is defined in sg::; this is vulkan's concrete realization on a pair of timeline
// semaphores. See libs/graphics/shaped-graphics/docs/concepts/epochs.md. Device-level teardown
// (shutdown) lives in vulkan_context.cc.

#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg::backend::vulkan
{
sg::epoch vulkan_context::completed_epoch() const
{
    // The epoch timeline's counter *is* the last fully-finished epoch — we signal the epoch value at
    // end-of-epoch. Its initial value is first-1, so before the first advance it reports first-1.
    cc::u64 const first_minus_one = cc::u64(sg::epoch::first) - 1;
    if (_epoch_timeline == VK_NULL_HANDLE)
        return sg::epoch(first_minus_one);
    cc::u64 value = 0;
    vkGetSemaphoreCounterValue(_device, _epoch_timeline, &value);
    return sg::epoch(value < cc::u64(sg::epoch::first) ? first_minus_one : value);
}

void vulkan_context::advance_epoch(cc::optional<int> allowed_in_flight)
{
    CC_ASSERT(!_is_shut_down, "cannot advance a shut-down context");
    CC_ASSERT(_open_command_lists.load(std::memory_order_relaxed) == 0, "all command lists opened this epoch must be "
                                                                        "submitted or dropped before advancing");

    sg::epoch const last = _current_epoch;
    _current_epoch = sg::epoch(cc::u64(last) + 1);

    // Signal end-of-epoch on the direct queue: an empty submit that raises the epoch timeline to `last`
    // once all of epoch `last`'s recorded GPU work has finished (the core invariant).
    cc::u64 const signal_value = cc::u64(last);
    auto const timeline_info = VkTimelineSemaphoreSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &signal_value,
    };
    auto const submit = VkSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_info,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &_epoch_timeline,
    };
    VkResult const r = vkQueueSubmit(_queue, 1, &submit, VK_NULL_HANDLE);
    if (r != VK_SUCCESS)
    {
        if (note_device_lost_if_lost(r, "epoch signal vkQueueSubmit"))
            throw sg::device_lost_exception(device_loss_reason());
        CC_ASSERT(false, "vkQueueSubmit (epoch signal) failed");
    }

    // Package everything `last` owns and push it onto the in-flight FIFO. (Externally synchronized, so
    // no submit races the pool drain; the lock is for correctness, not contention.)
    vulkan_epoch_data data;
    data.epoch_id = last;
    data.command_pools = _command_pools.lock(
        [](vulkan_command_pool_set& p)
        {
            cc::vector<vulkan_command_pool> out = cc::move(p.in_epoch);
            p.in_epoch = {};
            return out;
        });
    _epoch_state.lock(
        [&](vulkan_epoch_state& s)
        {
            data.expiring = cc::move(s.staged);
            s.staged = {};
            s.in_flight.push_back(cc::move(data));
        });

    // Throttle pipelining depth: keep at most `allowed_in_flight` epochs in flight.
    if (allowed_in_flight.has_value())
    {
        int const a = allowed_in_flight.value();
        CC_ASSERT(a >= 0, "allowed_in_flight must be non-negative");
        cc::u64 const allowed = cc::u64(a);
        cc::u64 const last_u = cc::u64(last);
        if (last_u >= cc::u64(sg::epoch::first) + allowed)
            wait_for_epoch(sg::epoch(last_u - allowed)); // this also retires
        else
            process_completed_epochs(); // too few epochs yet to wait on; still reclaim finished ones
    }
}

void vulkan_context::process_completed_epochs()
{
    if (_epoch_timeline == VK_NULL_HANDLE)
        return;
    cc::u64 completed = 0;
    vkGetSemaphoreCounterValue(_device, _epoch_timeline, &completed);

    // Drain finished epochs (oldest first, FIFO is sorted) under the lock; reclaim outside it.
    cc::vector<vulkan_epoch_data> done = _epoch_state.lock(
        [&](vulkan_epoch_state& s)
        {
            cc::vector<vulkan_epoch_data> out;
            for (auto& d : s.in_flight)
            {
                if (cc::u64(d.epoch_id) > completed)
                    break;
                out.push_back(cc::move(d));
            }
            s.in_flight.remove_from_to(0, out.size());
            return out;
        });

    // Pools are safe to reset now — every command buffer sourced from them has finished — so reset them
    // and return them to the free pool.
    _command_pools.lock(
        [&](vulkan_command_pool_set& p)
        {
            for (auto& e : done)
                for (auto& cp : e.command_pools)
                {
                    vkResetCommandPool(_device, cp.pool, 0);
                    p.free.push_back(cp);
                }
        });

    cc::vector<cc::unique_function<void()>> finalizers;
    for (auto& e : done)
        for (auto& res : e.expiring)
            release_expiring(_device, res, finalizers);

    // Run finalizers outside the lock — they may be slow or re-entrant, and the thread is not fixed.
    for (auto& f : finalizers)
        f();
}

void vulkan_context::wait_for_epoch(sg::epoch e)
{
    if (_epoch_timeline != VK_NULL_HANDLE)
    {
        cc::u64 const target = cc::u64(e);
        cc::u64 current = 0;
        vkGetSemaphoreCounterValue(_device, _epoch_timeline, &current);
        if (current < target)
        {
            auto const wait = VkSemaphoreWaitInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .semaphoreCount = 1,
                .pSemaphores = &_epoch_timeline,
                .pValues = &target,
            };
            VkResult const wr = vkWaitSemaphores(_device, &wait, UINT64_MAX);
            if (note_device_lost_if_lost(wr, "epoch semaphore wait"))
                throw sg::device_lost_exception(device_loss_reason());
        }
    }
    process_completed_epochs();
}

void vulkan_context::wait_for_next_inflight_epoch()
{
    cc::optional<sg::epoch> oldest = _epoch_state.lock(
        [](vulkan_epoch_state& s) -> cc::optional<sg::epoch>
        {
            if (s.in_flight.empty())
                return {};
            return s.in_flight.front().epoch_id;
        });
    if (oldest.has_value())
        wait_for_epoch(oldest.value());
}

bool vulkan_context::is_submission_complete(sg::submission_token token) const
{
    if (token == sg::submission_token::not_submitted)
        return false;
    if (_submission_timeline == VK_NULL_HANDLE)
        return true;
    cc::u64 value = 0;
    vkGetSemaphoreCounterValue(_device, _submission_timeline, &value);
    return value >= cc::u64(token);
}

void vulkan_context::schedule_deferred_deletion(vulkan_expiring_resource expiring)
{
    // Attributed to whatever epoch is open now; moved into that epoch's payload at the next advance.
    _epoch_state.lock([&](vulkan_epoch_state& s) { s.staged.push_back(cc::move(expiring)); });
}
} // namespace sg::backend::vulkan
