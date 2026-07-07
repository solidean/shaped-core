// dx12 epoch system: advance/retire, waits, and deferred-deletion staging. The epoch *concept*
// (counter + contract) is defined in sg::; this is dx12's concrete realization. See
// libs/graphics/shaped-graphics/docs/concepts/epochs.md. Device-level teardown (shutdown) lives in
// dx12_context.cc.

#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/raw_buffer.hh>
#include <shaped-graphics/raw_texture.hh>

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
    // Same again for the transient descriptor ring: snapshot `last`'s window, freed once `last` retires.
    _descriptor_heap.on_epoch_advance(last);

    // Auto-expire `last`'s transient buffers: their placed storage in ctx.transient's heap is reused by
    // the new epoch, so mark them expired now, which releases each resource into the deferred-deletion
    // staging area. Done before the staged drain below so those releases are attributed to `last`.
    // Outside any lock — expire() re-enters schedule_deferred_deletion, which takes _epoch_state.
    cc::vector<std::weak_ptr<sg::raw_buffer const>> const expiring_transient = _transient_expiring.lock(
        [](cc::vector<std::weak_ptr<sg::raw_buffer const>>& v)
        {
            auto out = cc::move(v);
            v.clear();
            return out;
        });
    for (auto& w : expiring_transient)
        if (auto const b = w.lock())
            b->expire();

    // Same for `last`'s transient textures (dedicated for now, but the transient contract still expires
    // them here, staging their GPU resources for deferred deletion attributed to `last`).
    cc::vector<std::weak_ptr<sg::raw_texture const>> const expiring_textures = _transient_expiring_textures.lock(
        [](cc::vector<std::weak_ptr<sg::raw_texture const>>& v)
        {
            auto out = cc::move(v);
            v.clear();
            return out;
        });
    for (auto& w : expiring_textures)
        if (auto const t = w.lock())
            t->expire();

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

    // Apply a pending ctx.transient.set_budget() now that the new epoch is open: this drains all in-flight
    // epochs and resizes the transient heap. Rare (only after a set_budget), so the stall is acceptable.
    apply_pending_transient_budget();

    // Same for the inline upload/download ring budgets (ctx.upload.set_inline_budget / ctx.download.
    // set_budget): each drains in-flight epochs — the download ring also waits out its actor — then
    // reallocates. No-op unless a budget change is pending.
    _upload_inline.apply_pending_budget();
    _download_inline.apply_pending_budget();
}

void dx12_context::process_completed_epochs()
{
    if (!_epoch_fence)
        return;
    cc::u64 const completed = _epoch_fence->GetCompletedValue();
    // Second release gate: how far the async upload copy queue has drained. A resource an in-flight async
    // upload still references must not be freed even after its epoch retired (the copy queue is decoupled
    // from epochs). `~0` when there is no copy fence, so the gate is trivially open.
    cc::u64 const copy_completed = _copy_fence ? _copy_fence->GetCompletedValue() : cc::u64(-1);

    // Reclaim inline upload ring space held by every epoch the GPU has now finished.
    _upload_inline.on_epochs_completed(sg::epoch(completed));
    // Reclaim transient descriptor ring slots the same way — the epoch fence proves the slots are idle.
    _descriptor_heap.on_epochs_completed(sg::epoch(completed));

    // Under the lock: drain finished epochs, then partition every expiring resource (from those epochs and
    // from the copy-deferred hold-back) by whether the copy queue has also passed its `copy_wait`. Ready
    // ones are released outside the lock; the rest stay on `copy_deferred` for the next sweep.
    cc::vector<dx12_epoch_data> done;
    cc::vector<dx12_expiring_resource> ready;
    _epoch_state.lock(
        [&](dx12_epoch_state& s)
        {
            for (auto& d : s.in_flight)
            {
                if (cc::u64(d.epoch_id) > completed)
                    break;
                done.push_back(cc::move(d));
            }
            s.in_flight.remove_from_to(0, done.size());

            auto const gate = [&](dx12_expiring_resource& r, cc::vector<dx12_expiring_resource>& not_ready)
            {
                if (cc::u64(r.copy_wait) <= copy_completed)
                    ready.push_back(cc::move(r));
                else
                    not_ready.push_back(cc::move(r));
            };

            // Re-check the existing hold-back: release what the copy queue has finished, keep the rest.
            cc::vector<dx12_expiring_resource> still_pending;
            for (auto& r : s.copy_deferred)
                gate(r, still_pending);
            s.copy_deferred = cc::move(still_pending);

            // Retired epochs' resources: release now, or hold back if the copy queue is still behind.
            for (auto& e : done)
                for (auto& r : e.expiring)
                    gate(r, s.copy_deferred);
        });

    // Allocators are safe to reset now — every command list sourced from them has finished — so hand
    // them back to the pool, which resets each and returns it to its queue's free list.
    for (auto& e : done)
        _cmd_pool.reclaim_allocators(cc::move(e.allocators));

    cc::vector<cc::unique_function<void()>> finalizers;
    for (auto& r : ready)
        release_expiring(r, finalizers);

    // Run finalizers outside the lock — they may be slow or re-entrant, and the thread is not fixed.
    for (auto& f : finalizers)
        f();
}

void dx12_context::wait_for_epoch(sg::epoch e)
{
    if (_epoch_fence)
    {
        cc::u64 const target = cc::u64(e);
        if (_epoch_fence->GetCompletedValue() < target)
        {
            // A per-call event, not a shared one: the wait/retire family is safe to call from any thread
            // (e.g. ring back-pressure during concurrent recording), and a single reused event cannot
            // serve concurrent waiters. A fence accepts many concurrent SetEventOnCompletion registrations.
            HANDLE const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            CC_ASSERT(event != nullptr, "CreateEventW failed for the epoch fence wait");
            HRESULT const hr = _epoch_fence->SetEventOnCompletion(target, event);
            CC_ASSERT(SUCCEEDED(hr), "ID3D12Fence::SetEventOnCompletion failed");
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
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
