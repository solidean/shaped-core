// dx12 epoch system: advance/retire, waits, and deferred-deletion staging. The epoch *concept*
// (counter + contract) is defined in sg::; this is dx12's concrete realization. See
// libs/graphics/shaped-graphics/docs/concepts/epochs.md. Device-level teardown (shutdown) lives in
// dx12_context.cc.

#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
sg::epoch dx12_context::completed_epoch() const
{
    // The epoch fence's completed value *is* the last fully-finished epoch — we signal the epoch
    // value at end-of-epoch. Before the first advance it reads 0, so report first-1.
    cc::u64 const first_minus_one = cc::u64(sg::epoch::first) - 1;
    if (!_epoch_fence)
        return sg::epoch(first_minus_one);
    cc::u64 const v = _epoch_fence->GetCompletedValue();
    return sg::epoch(v < cc::u64(sg::epoch::first) ? first_minus_one : v);
}

void dx12_context::advance_epoch(cc::optional<int> allowed_in_flight)
{
    CC_ASSERT(!_is_shut_down, "cannot advance a shut-down context");
    CC_ASSERT(_open_command_lists.load(std::memory_order_relaxed) == 0, "all command lists opened this epoch must be "
                                                                        "submitted or dropped before advancing");

    sg::epoch const last = _current_epoch;
    _current_epoch = sg::epoch(cc::u64(last) + 1);

    // Snapshot the inline upload ring cursor as `last`'s boundary; its space frees once `last` retires.
    _upload_inline.on_epoch_advance(last);
    // Same for the inline download ring, but its span frees once the actor drains `last`'s readback
    // copies (tracked per-epoch), not at GPU retire — so the hook is only needed here, not in retire.
    _download_inline.on_epoch_advance(last);
    // Same again for the transient buffer heap: snapshot `last`'s window, freed once `last` retires.
    _transient_buffers.on_epoch_advance(last);

    // Signal end-of-epoch: enqueues "epoch `last` finished" after all of its recorded GPU work.
    HRESULT const hr = _queue->Signal(_epoch_fence.Get(), cc::u64(last));
    CC_ASSERT(SUCCEEDED(hr), "ID3D12CommandQueue::Signal failed");

    // Package everything `last` owns and push it onto the in-flight FIFO. (Externally synchronized, so
    // no submit races the allocator drain; the lock is for correctness, not contention.)
    dx12_epoch_data data;
    data.epoch_id = last;
    data.allocators = _cmd_pool.drain_in_epoch_allocators();
    _epoch_state.lock(
        [&](dx12_epoch_state& s)
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

void dx12_context::process_completed_epochs()
{
    if (!_epoch_fence)
        return;
    cc::u64 const completed = _epoch_fence->GetCompletedValue();

    // Reclaim inline upload ring space held by every epoch the GPU has now finished.
    _upload_inline.on_epochs_completed(sg::epoch(completed));
    // Reclaim transient buffer heap windows the same way — the epoch fence proves the memory is idle.
    _transient_buffers.on_epochs_completed(sg::epoch(completed));

    // Drain finished epochs (oldest first, FIFO is sorted) under the lock; reclaim outside it.
    cc::vector<dx12_epoch_data> done = _epoch_state.lock(
        [&](dx12_epoch_state& s)
        {
            cc::vector<dx12_epoch_data> out;
            for (auto& d : s.in_flight)
            {
                if (cc::u64(d.epoch_id) > completed)
                    break;
                out.push_back(cc::move(d));
            }
            s.in_flight.remove_from_to(0, out.size());
            return out;
        });

    // Allocators are safe to reset now — every command list sourced from them has finished — so hand
    // them back to the pool, which resets each and returns it to its queue's free list.
    for (auto& e : done)
        _cmd_pool.reclaim_allocators(cc::move(e.allocators));

    cc::vector<cc::unique_function<void()>> finalizers;
    for (auto& e : done)
        for (auto& r : e.expiring)
            release_expiring(r, finalizers);

    // Run finalizers outside the lock — they may be slow or re-entrant, and the thread is not fixed.
    for (auto& f : finalizers)
        f();
}

void dx12_context::wait_for_epoch(sg::epoch e)
{
    if (_epoch_fence && _fence_event)
    {
        cc::u64 const target = cc::u64(e);
        if (_epoch_fence->GetCompletedValue() < target)
        {
            HRESULT const hr = _epoch_fence->SetEventOnCompletion(target, _fence_event);
            CC_ASSERT(SUCCEEDED(hr), "ID3D12Fence::SetEventOnCompletion failed");
            WaitForSingleObject(_fence_event, INFINITE);
        }
    }
    process_completed_epochs();
}

void dx12_context::wait_for_next_inflight_epoch()
{
    cc::optional<sg::epoch> oldest = _epoch_state.lock(
        [](dx12_epoch_state& s) -> cc::optional<sg::epoch>
        {
            if (s.in_flight.empty())
                return {};
            return s.in_flight.front().epoch_id;
        });
    if (oldest.has_value())
        wait_for_epoch(oldest.value());
}

bool dx12_context::is_submission_complete(sg::submission_token token) const
{
    if (token == sg::submission_token::not_submitted)
        return false;
    if (!_submission_fence)
        return true;
    return _submission_fence->GetCompletedValue() >= cc::u64(token);
}

void dx12_context::schedule_deferred_deletion(dx12_expiring_resource expiring)
{
    // Attributed to whatever epoch is open now; moved into that epoch's payload at the next advance.
    _epoch_state.lock([&](dx12_epoch_state& s) { s.staged.push_back(cc::move(expiring)); });
}
} // namespace sg::backend::dx12
