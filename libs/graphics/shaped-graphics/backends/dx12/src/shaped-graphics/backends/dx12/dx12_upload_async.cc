// dx12_upload_async_system: the copy actor behind dx12_upload_async.hh — window packing, submission, and the staging memcpy.
// The shape and the two fences are on the class doc there.
// Source bytes are read only during the memcpy into staging, so a job and its pin die as soon as it is fully staged — on the actor thread, off the submission path.
// Why the acyclicity guard is load-bearing: libs/graphics/shaped-graphics/docs/concepts/upload.async.md.

#include <clean-core/common/time.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_resource_upload.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
#include <shaped-graphics/backends/dx12/dx12_upload_async.hh>
#include <shaped-graphics/transfer/impl/transfer_scheduler.hh>

namespace sg::backend::dx12
{
namespace
{
// Triple-buffered staging: one window being copied by the GPU, one just submitted, one being filled by the CPU.
// Fewer than three reintroduces a sync bubble — the CPU would stall on the window it just handed the GPU.
// More only adds staging memory.
constexpr int num_staging_windows = 3;

// A window size rounds up to the texture placement alignment (512), so each window's base is 512-aligned.
// A texture copy's placed footprint must start there; buffers are unaffected by the rounding.
[[nodiscard]] isize round_window(isize bytes)
{
    return (bytes + texture_placement_alignment - 1) / texture_placement_alignment * texture_placement_alignment;
}

// Raise `slot` to `value`, never lower it.
// Every cross-queue stamp is monotonic and never reset, so a racing higher value simply wins and a stale one yields a
// cheap already-satisfied wait.
void stamp_max(std::atomic<u64>& slot, u64 value)
{
    u64 prev = slot.load(std::memory_order_relaxed);
    while (prev < value && !slot.compare_exchange_weak(prev, value, std::memory_order_release, std::memory_order_relaxed))
    {
        // CAS retries; `prev` is refreshed with the current value each time.
    }
}

// Fires on a thread-pool thread once the completion fence reaches a value a queued settle is waiting on.
// It does nothing but wake the actor — _pending_settles is actor-thread state, and settling from here would race it.
// Routed through the waker rather than the system directly, since that is the one entry point already safe at any
// point in teardown.
void CALLBACK on_settle_fence_signaled(PVOID context, BOOLEAN /*timed_out*/)
{
    static_cast<dx12_upload_waker*>(context)->wake();
}

/// A finished streaming upload waiting only for the GPU to run its copies before it may report complete.
struct pending_settle
{
    dx12_group_value completion; // the value, on the destination's own timeline, that the copy signals
    std::shared_ptr<sg::impl::stream_control> control;
};

/// The highest completion value the open window finished on one timeline.
/// A window may finish transfers to several destinations, and each has to be signaled on its own fence — one
/// Signal per entry after the window executes.
struct open_completion
{
    dx12_completion_group_handle group;
    u64 value = 0;
};

/// A job the actor has resolved and is packing.
///
/// The packer carries the per-job cursor, so a job survives across windows AND across process cycles.
/// That is what lets the actor pick a different job between chunks rather than draining each one to completion
/// before it looks at the next, which is the whole basis of out-of-order selection.
///
/// `keepalive` holds the destination strong for exactly as long as the packer names its raw `ID3D12Resource*`.
/// The job's own handle is weak, so without it the storage could die mid-pack.
struct active_upload
{
    dx12_async_upload_job job;
    std::unique_ptr<dx12_resource_upload> packer;
    std::shared_ptr<void const> keepalive;
    u64 sequence = 0; // actor-assigned submission order
    u64 family = 0;   // destination resource: same-family jobs must stay in sequence order

    // When the actor took this job on, for aging.
    // Read once per window rather than per candidate: aging changes an effective priority continuously, but the
    // scheduler only acts on it when a window opens, so a finer reading would cost clock reads to no effect.
    double admitted_at = 0;

    // Source-driven jobs only: the chunk sequence, and the pin keeping the CURRENT chunk's bytes alive while its
    // packer reads them window by window.
    std::unique_ptr<sg::stream_source> source;
    cc::pinned_data<byte const> current_chunk;
    ID3D12Resource* resource = nullptr; // the destination, kept alive by `keepalive`; each chunk builds a packer on it

    // Set when the source answered `not_yet` this cycle, so the scheduler passes the job over instead of spinning
    // on it.
    // Cleared at the top of every cycle, which is what makes a wake re-poll.
    bool stalled = false;
};

/// The async-upload copy actor: one thread that packs jobs into staging windows and submits copy work.
/// All window / job / command-list state lives here and is touched only on the actor thread, so it needs no locks.
/// It reaches the shared, immutable-after-init fields (staging buffer, fences, queue) via _sys.
class dx12_upload_async_actor final : public cc::threaded_actor_impl<dx12_async_upload_job, dx12_transfer_wake>
{
public:
    explicit dx12_upload_async_actor(dx12_upload_async_system& sys) : _sys(sys) {}

    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "sg-dx12-upload-async"; }

protected:
    void on_message(dx12_async_upload_job job) override { _pending.push_back(cc::move(job)); }

    // A wake carries nothing: arriving at all is the signal, and the cycle it triggers re-polls every source.
    void on_message(dx12_transfer_wake) override {}

    bool on_process() override
    {
        maybe_resize_staging(); // adopt a pending set_window_bytes now, while no window is open

        drain_ready_settles(); // whatever the GPU finished while this actor slept

        // A fresh cycle re-polls every stalled source: this cycle exists because something changed, and the source
        // is the only one who knows whether it was its own data arriving.
        for (auto& a : _active)
            a.stalled = false;

        admit_pending();
        pack_until_stalled();
        submit_window();       // flush the final partial window so its copies run
        drain_ready_settles(); // and anything this cycle's copies have already covered

        // Unthreaded, no later cycle is promised: the pump registry sweeps only while an actor reports work, and it
        // is about to be told there is none.
        // So finish the job here — the caller is already blocking, and with no actor thread there is no other
        // transfer for the wait to starve.
        if (_sys.actor_is_unthreaded())
            drain_settles_blocking();
        else
            arm_settle_wake(); // threaded: a fence event is the only thing that will wake us for the rest

        return false; // everything staged + submitted; sleep until a message or that event
    }

    void on_thread_shutdown() override
    {
        // Flush anything still buffered, then wait for the copy queue to idle.
        // The staging buffer and the command list/allocators are only safe to release afterwards.
        // This queue is independent of the direct queue, which shutdown drains separately.
        admit_pending();
        pack_until_stalled();

        // Anything still active at shutdown is cancelled rather than abandoned: a handle waiting on a node nobody
        // will ever push would hang the process it is trying to tear down.
        for (auto& a : _active)
            cancel_stream(a.job);
        _active.clear();

        submit_window();
        drain_settles_blocking();
        if (_current_window > 0)
            wait_for_window(_current_window - 1); // the last submitted window
    }

private:
    // A dropped upload — every handle released, or the storage expired, before the actor reached it.
    // Skip the copy: a 1 GiB upload to a released buffer must not stage or block.
    // But STILL advance the completion fence to this job's value.
    // The lifetime gate holds the storage until the fence reaches it, and any forward reader stamped with it waits on the same value — a hole would hang both.
    // An empty window still submits and signals, keeping the completion fence monotonic and gap-free.
    void fold_dropped_completion(dx12_async_upload_job& job)
    {
        // A streaming job also has to say so out loud: its handle, or a completion() taken from it, may still be
        // waiting, and a manual node nobody pushes never wakes its dependents.
        cancel_stream(job);
    }

    // Resolves each arriving job's destination and gives it a resumable packer.
    // A destination already released skips the copy entirely — a 1 GiB upload to a dead buffer must not stage.
    void admit_pending()
    {
        // One reading for the whole batch: these all arrive together, and dating them apart by microseconds would
        // only make the aging order depend on clock noise.
        double const admitted_at = cc::current_time_steady_secs();
        for (auto& job : _pending)
        {
            active_upload a;
            bool const source_driven = job.source != nullptr;
            if (job.is_texture)
            {
                auto strong = job.texture_target.lock();
                if (!strong || !strong->_resource)
                {
                    fold_dropped_completion(job);
                    continue;
                }
                CC_ASSERT(round_window(job.footprint.padded_pitch) <= _sys._window_bytes, "a single texture row "
                                                                                          "exceeds one staging "
                                                                                          "window");
                a.family = u64(reinterpret_cast<u64>(strong->_resource.Get()));
                a.resource = strong->_resource.Get();
                if (!source_driven)
                    a.packer = std::make_unique<dx12_texture_upload>(a.resource, job.footprint, job.src.span());
                a.keepalive = cc::move(strong);
            }
            else
            {
                auto strong = job.buffer_target.lock();
                if (!strong || !strong->_resource)
                {
                    fold_dropped_completion(job);
                    continue;
                }
                a.family = u64(reinterpret_cast<u64>(strong->_resource.Get()));
                a.resource = strong->_resource.Get();
                if (!source_driven)
                    a.packer = std::make_unique<dx12_buffer_upload>(a.resource, job.dst_offset, job.src.span());
                a.keepalive = cc::move(strong);
            }
            a.sequence = _next_sequence++;
            a.admitted_at = admitted_at;
            a.source = cc::move(job.source);
            if (a.source)
            {
                if (auto const hint = a.source->total_size_hint(); hint >= 0 && job.stream)
                    job.stream->total_hint.store(hint, std::memory_order_relaxed);
                // Null once shutdown has detached it, and shutdown detaches BEFORE draining the actor — so a job
                // admitted on the shutdown path gets no waker rather than one that dereferences null.
                // It needs none: nothing after this point will open another window for it.
                if (_sys._waker != nullptr)
                    a.source->set_waker([waker = _sys._waker] { waker->wake(); });
            }
            a.job = cc::move(job);
            _active.push_back(cc::move(a));
        }
        _pending.clear();
    }

    // Folds a job's completion value into the open window.
    // Mandatory on every exit path, delivered or not: the fence is monotonic, and both the deferred-deletion gate
    // and any forward reader stamped with the value wait on it, so a hole would hang them.
    void fold_completion_value(dx12_async_upload_job const& job)
    {
        if (!job.completion.is_pending())
            return;
        ensure_open_window();
        for (auto& f : _open_finished)
            if (f.group == job.completion.group)
            {
                if (job.completion.value > f.value)
                    f.value = job.completion.value;
                return;
            }
        _open_finished.push_back(open_completion{job.completion.group, job.completion.value});
    }

    // Fails a streaming job's completion node, exactly once.
    // Saying it out loud is mandatory: a manual async nobody pushes parks its dependents for the process's lifetime.
    void cancel_stream(dx12_async_upload_job& job)
    {
        if (job.stream)
        {
            job.stream->completion->push_error(cc::async_error::make_cancelled());
            job.stream = nullptr;
        }
        fold_completion_value(job);
    }

    // Queues a finished streaming job to settle once the GPU has actually run its copies.
    //
    // Recording the last chunk is NOT completion.
    // The handle's whole promise is that once it reports complete, a command list touching the streamed extent
    // may be submitted — and that is only true after the copy has run.
    // Settling at record time reads as working and silently hands back a half-written buffer, which is exactly what
    // the round-trip test caught.
    // Async uploads have no such step because their completion is a GPU-side fence wait rather than a CPU signal.
    void queue_stream_settle(dx12_async_upload_job& job)
    {
        fold_completion_value(job);
        if (job.stream)
        {
            _pending_settles.push_back(pending_settle{job.completion, cc::move(job.stream)});
            job.stream = nullptr;
        }
    }

    // Settles every finished stream whose copy the GPU has actually run, and leaves the rest queued.
    //
    // Deliberately does NOT block.
    // This actor stages every other transfer in the system, so sitting on a fence here would stall all of them
    // behind one stream's copy — and the stream has nothing to gain from being told a cycle earlier.
    // What a settle does need is a wake once its value lands, which arm_settle_wake provides.
    void drain_ready_settles()
    {
        if (_pending_settles.empty())
            return;

        cc::vector<pending_settle> still_pending;
        for (auto& p : _pending_settles)
        {
            if (p.completion.has_reached())
                p.control->completion->push_value(cc::unit{});
            else
                still_pending.push_back(cc::move(p));
        }
        _pending_settles = cc::move(still_pending);
    }

    // Asks the completion fence to signal this system's settle event once it reaches the highest value a queued
    // settle is waiting on; the event's registered wait then wakes the actor.
    //
    // Without it a stream settles only when some unrelated message happens to arrive — constantly in a busy system,
    // and never in a quiet one, which is exactly the case where a caller is blocked on its completion.
    // Re-armed every cycle rather than tracked, since an already-passed value is settled above and never reaches here.
    void arm_settle_wake()
    {
        if (_pending_settles.empty() || _sys._settle_event == nullptr)
            return;

        // One registration per timeline still owed: any of them signaling wakes the actor, which re-drains and
        // re-arms whatever is left.
        for (auto const& p : _pending_settles)
        {
            HRESULT const hr = p.completion.group->fence->SetEventOnCompletion(p.completion.value, _sys._settle_event);
            CC_ASSERT(SUCCEEDED(hr), "ID3D12Fence::SetEventOnCompletion (stream settle) failed");
        }
    }

    // Teardown's blocking twin: nothing may be left unsettled once the actor stops, and a handle waiting on a node
    // nobody will ever push would hang the very shutdown trying to finish.
    // Blocking is right here and only here — this path already waits the copy queue out anyway.
    void drain_settles_blocking()
    {
        if (_pending_settles.empty())
            return;

        for (auto const& p : _pending_settles)
            wait_for_group_value(p.completion);
        drain_ready_settles();
    }

    // Blocks the actor until `v`'s timeline has reached it — i.e. the window carrying that copy has run.
    // Teardown only; the steady-state path never blocks here (see drain_ready_settles).
    void wait_for_group_value(dx12_group_value const& v)
    {
        if (v.has_reached())
            return;
        HRESULT const hr = v.group->fence->SetEventOnCompletion(v.value, _sys._wait_event);
        CC_ASSERT(SUCCEEDED(hr), "ID3D12Fence::SetEventOnCompletion (completion) failed");
        WaitForSingleObject(_sys._wait_event, INFINITE);
    }

    // Drops every active streaming job whose handle has cancelled it.
    // Cancellation is a flag rather than queue surgery: the job simply stops being picked, and is reaped here when
    // the actor next looks.
    // Chunks already recorded still run — their staging bytes are committed.
    void reap_cancelled()
    {
        for (isize i = _active.size() - 1; i >= 0; --i)
        {
            auto& a = _active[i];
            if (a.job.stream == nullptr || !a.job.stream->cancelled.load(std::memory_order_relaxed))
                continue;
            cancel_stream(a.job);
            _active.remove_from_to(i, i + 1);
        }
    }

    // How the scheduler sees one active job against the currently open window.
    //
    // A window issues its reverse-sync wait ONCE, hoisted to the front (submit_window), so it must never both
    // promise a completion V and carry a reverse wait that could depend on V.
    // The hoisted Wait would then sit ahead of the very copy whose signal it needs — the copy-actor deadlock.
    //
    // Two eligibility rules keep that true INDEPENDENT of the order jobs are packed in, which matters now that the
    // order is no longer submission order:
    //  - a job whose reverse token is still pending may not join a window that has already finished an upload;
    //  - once such a job IS in the open window, no other job may join, so no other job's completion can land beside
    //    its wait.
    // A job's own completion is safe beside its own wait: the token was read before the value was reserved, so
    // nothing that token names can depend on that value.
    // Under the old strictly-FIFO staging the first rule alone covered both cases; it stops sufficing once jobs
    // interleave, because the pending-wait job can now be packed BEFORE the one that finishes.
    [[nodiscard]] sg::impl::transfer_candidate candidate_for(active_upload const& a) const
    {
        sg::impl::transfer_candidate c;
        c.family = a.family;
        c.sequence = a.sequence;
        if (a.job.stream)
        {
            c.flavor = sg::impl::transfer_flavor::streaming;
            c.priority = a.job.stream->priority.load(std::memory_order_relaxed);
        }

        c.age_seconds = float(_window_opened_at - a.admitted_at);

        if (a.stalled) // its source has nothing right now; fill the window with other work rather than spin
            c.eligible = false;

        bool const pending_wait = u64(a.job.wait_token) > _sys._ctx._submission_fence->GetCompletedValue();
        if (pending_wait && !_open_finished.empty())
            c.eligible = false;
        if (_open_risky_job.has_value() && _open_risky_job.value() != a.sequence)
            c.eligible = false;
        return c;
    }

    // Picks and packs one chunk at a time until nothing can make progress in any window.
    // A job that cannot go is passed over rather than blocking the queue behind it, which is what removes
    // head-of-line blocking: one upload waiting on a slow command list no longer stalls every later one.
    void pack_until_stalled()
    {
        reap_cancelled();
        while (!_active.empty())
        {
            ensure_open_window();

            _candidates.clear();
            for (auto const& a : _active)
                _candidates.push_back(candidate_for(a));

            auto const pick = _sys._scheduler.pick_next(_candidates);
            if (!pick.has_value())
            {
                // Nothing fits THIS window.
                // Closing it clears both eligibility blocks, so a fresh one can take what this one could not.
                //
                // "Pristine" has to mean no folded completion either, not just no bytes.
                // A batch whose first jobs were all dropped folds their values into an otherwise empty window, and
                // that alone blocks every pending-wait job.
                // Stopping there would leave those jobs unstaged and their completion values never signaled — a
                // fence hole that hangs the direct queue waiting on it.
                if (_window_used == 0 && _open_finished.empty())
                    break;
                submit_window();
                continue;
            }

            isize const index = pick.value();
            if (_active[index].packer == nullptr && !pull_next_chunk(index))
                continue; // stalled, finished or failed — all handled inside, so just re-pick

            isize const chunk_index = pick.value();
            if (!pack_chunk(_active[chunk_index]))
            {
                CC_ASSERT(_window_used > 0, "an empty staging window could not fit a single chunk");
                submit_window(); // window tail too small for the next aligned texture row → roll to a fresh one
                continue;
            }

            auto& packed = _active[chunk_index];
            if (packed.packer->is_finished())
            {
                packed.packer = nullptr; // this chunk is recorded; ask the source for the next one
                if (packed.source == nullptr)
                    finish_job(chunk_index); // the resident-payload form has exactly one chunk
            }

            // Independent of whether that finished the job: one ending exactly on the window boundary still leaves
            // a full window, and the next pick would be handed a zero-byte allocation.
            if (_window_used == _sys._window_bytes)
                submit_window();
        }

        // On a pristine window every job whose source can answer is eligible, so the loop only ends once each of
        // them is staged.
        // What may legitimately remain is a job waiting on a source — its own, or that of an earlier job in its
        // family, which family order holds it behind.
        // Both resume on the cycle that source's waker starts.
        // Anything else left here would never signal its completion value, and the first direct-queue list waiting on
        // one would hang — so say it loudly rather than deadlocking in the dark.
        for (auto const& a : _active)
            CC_ASSERT(waits_on_a_source(a), "async upload actor stalled with a job no source is holding up");
    }

    // Whether `a` may legitimately still be active once nothing more can be packed.
    //
    // Its own source having nothing is the obvious case.
    // The other one is a job held by FAMILY ORDER behind a stalled job to the same destination — an async upload
    // queued after a streaming one to the same buffer is the ordinary way to get there, and it is not a fault: both
    // write that buffer, so the order between them is exactly what must be kept.
    [[nodiscard]] bool waits_on_a_source(active_upload const& a) const
    {
        if (a.stalled)
            return true;
        for (auto const& other : _active)
            if (other.stalled && other.family == a.family && other.sequence < a.sequence)
                return true;
        return false;
    }

    // Asks a source-driven job's source for its next chunk and builds a packer for it.
    // False when nothing was packed — the job stalled, finished or failed, each fully handled here.
    //
    // Polling happens here rather than in candidate_for because a poll CONSUMES a chunk: asking is not a read-only
    // question, so it may only be asked of the job actually about to be served.
    [[nodiscard]] bool pull_next_chunk(isize index)
    {
        auto& a = _active[index];
        CC_ASSERT(a.source != nullptr, "only a source-driven job pulls chunks");

        sg::stream_poll poll = a.source->try_next_chunk();
        switch (poll.status)
        {
        case sg::stream_source_status::ready:
            break;

        case sg::stream_source_status::not_yet:
            a.stalled = true; // passed over until the next cycle, which a wake or any other message starts
            return false;

        case sg::stream_source_status::done:
            finish_job(index);
            return false;

        case sg::stream_source_status::failed:
            fail_job(index);
            return false;
        }

        // An empty `ready` chunk would make no progress and would be picked again immediately, so treat it as the
        // stall it effectively is rather than looping on it.
        if (poll.chunk.data.empty())
        {
            a.stalled = true;
            return false;
        }

        // The pin has to outlive the packer, which reads the bytes window by window rather than all at once.
        a.current_chunk = cc::move(poll.chunk.data);

        if (a.job.is_texture)
        {
            auto const& fp = a.job.footprint;
            CC_ASSERT(fp.row_bytes > 0, "texture stream footprint has no rows");
            CC_ASSERT(poll.chunk.offset % fp.row_bytes == 0, "a texture stream chunk must start on a row boundary");
            a.packer = std::make_unique<dx12_texture_upload>(a.resource, fp, a.current_chunk.span(),
                                                             poll.chunk.offset / fp.row_bytes);
        }
        else
        {
            a.packer = std::make_unique<dx12_buffer_upload>(a.resource, a.job.dst_offset + poll.chunk.offset,
                                                            a.current_chunk.span());
        }
        return true;
    }

    // Completes a job: folds its completion value, queues its settle behind the copy fence, and drops it.
    void finish_job(isize index)
    {
        auto& a = _active[index];
        if (a.job.stream)
            queue_stream_settle(a.job);
        else
            fold_completion_value(a.job);
        _active.remove_from_to(index, index + 1); // releases the pins + keepalive, on the actor thread
    }

    // Fails a job whose source gave up.
    // Its already-recorded chunks still run; nothing further is served.
    void fail_job(isize index)
    {
        auto& a = _active[index];
        cancel_stream(a.job);
        _active.remove_from_to(index, index + 1);
    }

    // Writes one chunk of `a` into the open window and records its copy.
    // False when the window tail cannot fit the job's next aligned chunk; the caller rolls to a fresh window.
    [[nodiscard]] bool pack_chunk(active_upload& a)
    {
        isize const avail = _sys._window_bytes - _window_used;
        isize const base = isize(_current_window % u64(num_staging_windows)) * _sys._window_bytes;
        dx12_upload_allocation const alloc = {_sys._staging.Get(), _sys._mapped, base + _window_used, avail};

        isize const consumed = a.packer->execute_next_job(*_list.Get(), alloc);
        if (consumed == 0)
            return false;
        _window_used += consumed;
        if (a.job.stream)
        {
            _window_stream_bytes += consumed;
            a.job.stream->bytes_done.fetch_add(consumed, std::memory_order_relaxed);
        }
        else
            _window_async_bytes += consumed;

        // This chunk writes the destination, so its window must first wait for the last direct-queue list that used it.
        // Max over the window; the submission fence is monotonic.
        if (u64(a.job.wait_token) > _open_max_wait_token)
            _open_max_wait_token = u64(a.job.wait_token);
        if (u64(a.job.wait_token) > _sys._ctx._submission_fence->GetCompletedValue())
            _open_risky_job = a.sequence; // the window is now dedicated to it — see candidate_for

        // Completing is NOT this function's business, even when the packer just finished: one packer covers one
        // chunk, and a source-driven job has as many as its source cares to hand over.
        // finish_job decides, and it is the caller who knows whether the source has more to say.
        return true;
    }

    // Ensures a window is open with room to write.
    // One command list is reused across all windows: Reset onto the next allocator is legal while a prior submission still executes.
    // Each of the three window slots has its own allocator.
    // Reusing a slot waits on the window three submissions back, so both that allocator's GPU work and its staging memory are done.
    void ensure_open_window()
    {
        if (_window_open)
            return;

        int const slot = int(_current_window % u64(num_staging_windows));
        if (_current_window >= u64(num_staging_windows))
            wait_for_window(_current_window - u64(num_staging_windows));

        if (_allocators[slot] == nullptr) // first use of this slot: fresh allocator, ready to record
        {
            HRESULT const ha = _sys._ctx._device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                                         IID_PPV_ARGS(&_allocators[slot]));
            CC_ASSERT(SUCCEEDED(ha), "ID3D12Device::CreateCommandAllocator (copy) failed");
        }
        else // reuse: the window that last used this allocator has completed (waited above)
        {
            HRESULT const ra = _allocators[slot]->Reset();
            CC_ASSERT(SUCCEEDED(ra), "ID3D12CommandAllocator::Reset failed");
        }

        if (_list == nullptr) // created once, in the recording state
        {
            HRESULT const hl = _sys._ctx._device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_COPY, _allocators[slot].Get(), nullptr, IID_PPV_ARGS(&_list));
            CC_ASSERT(SUCCEEDED(hl), "ID3D12Device::CreateCommandList (copy) failed");
        }
        else // reset the shared list onto this window's allocator
        {
            HRESULT const rl = _list->Reset(_allocators[slot].Get(), nullptr);
            CC_ASSERT(SUCCEEDED(rl), "ID3D12GraphicsCommandList::Reset failed");
        }

        _window_used = 0;
        _open_finished.clear();
        _open_max_wait_token = 0;
        _open_risky_job = {};
        _window_async_bytes = 0;
        _window_stream_bytes = 0;
        _window_opened_at = cc::current_time_steady_secs();
        _sys._scheduler.begin_window();
        _window_open = true;
    }

    // Closes + submits the open window: executes it, then signals the window fence — its staging memory and allocator are reusable once the GPU drains it.
    // If the window finished any upload it also signals the completion fence up to the highest finished value.
    // No-op when no window is open.
    void submit_window()
    {
        if (!_window_open)
            return;

        HRESULT const hc = _list->Close();
        CC_ASSERT(SUCCEEDED(hc), "ID3D12GraphicsCommandList::Close failed");

        // Reverse sync: hold the copy queue until every direct-queue list that used this window's destinations has finished.
        // The copy then never races an earlier-submitted reader or writer.
        // Over-waiting on a higher (monotonic) token is safe, and an already-completed token returns at once.
        if (_open_max_wait_token > 0)
            _sys._copy_queue->Wait(_sys._ctx._submission_fence.Get(), _open_max_wait_token);

        ID3D12CommandList* lists[] = {_list.Get()};
        _sys._copy_queue->ExecuteCommandLists(1, lists);

        // Window fence values are 1-based: window i completing signals i+1.
        // The fence starts at 0, so a 0-based value for window 0 would read as "not yet started" and wait_for_window(0) would never block — recycling slot 0 before its copy drained it.
        // wait_for_window applies the same +1, so callers pass window indices.
        HRESULT const hs = _sys._copy_queue->Signal(_sys._window_fence.Get(), _current_window + 1);
        CC_ASSERT(SUCCEEDED(hs), "ID3D12CommandQueue::Signal (staging window) failed");

        // One Signal per timeline this window finished an upload on, each monotonic on its own fence.
        // A window carrying only mid-upload chunks finishes nothing and signals nothing.
        // Signaling the max PER TIMELINE is exact because the scheduler keeps same-destination jobs in sequence
        // order, so within a group completion order is reservation order.
        for (auto const& f : _open_finished)
        {
            if (f.value <= f.group->last_signaled)
                continue;
            HRESULT const hcf = _sys._copy_queue->Signal(f.group->fence.Get(), f.value);
            CC_ASSERT(SUCCEEDED(hcf), "ID3D12CommandQueue::Signal (completion) failed");
            f.group->last_signaled = f.value;
        }

        _sys._scheduler.on_window_submitted(_window_async_bytes, _window_stream_bytes);
        _window_open = false;
        ++_current_window;
    }

    // Adopts a pending set_window_bytes when one differs from the current size.
    // Called at the top of a process cycle, when no window is open — any open window is submitted first.
    // Fully drains the copy queue so no in-flight window still reads the old staging buffer, then rebuilds it at the new size.
    // The per-slot allocators and the reused command list survive; only staging memory changes.
    void maybe_resize_staging()
    {
        isize const desired = round_window(_sys._desired_window_bytes.load(std::memory_order_acquire));
        if (desired == _sys._window_bytes)
            return;
        CC_ASSERT(desired > 0, "async upload staging window must be positive");

        submit_window(); // close any open window before draining
        if (_current_window > 0)
            wait_for_window(_current_window - 1); // wait out every submitted window (drops all readers)

        // Build the new staging buffer before releasing the old, so a failed allocation leaves the current one intact.
        // The resize is then simply not applied, and the next cycle retries.
        auto ring = create_mapped_ring_buffer(_sys._ctx._device.Get(), D3D12_HEAP_TYPE_UPLOAD,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, desired * num_staging_windows);
        CC_ASSERT(ring.has_value(), "async upload staging resize failed to allocate");

        _sys._staging->Unmap(0, nullptr);
        _sys._staging = cc::move(ring.value().resource);
        _sys._mapped = static_cast<byte*>(ring.value().mapped);
        _sys._window_bytes = desired;
        _sys._scheduler.set_window_bytes(desired);
    }

    // Blocks the actor until the copy queue has finished `window` (index).
    // The fence is 1-based (see submit_window), so window i's completion is fence value i+1 — distinct from the initial 0.
    void wait_for_window(u64 window)
    {
        u64 const target = window + 1;
        if (_sys._window_fence->GetCompletedValue() < target)
        {
            HRESULT const hr = _sys._window_fence->SetEventOnCompletion(target, _sys._wait_event);
            CC_ASSERT(SUCCEEDED(hr), "ID3D12Fence::SetEventOnCompletion failed");
            WaitForSingleObject(_sys._wait_event, INFINITE);
        }
    }

    dx12_upload_async_system& _sys;

    cc::vector<dx12_async_upload_job> _pending;           // received since the last cycle, resolved in admit_pending
    cc::vector<active_upload> _active;                    // resolved and mid-pack, until the last chunk is recorded
    cc::vector<sg::impl::transfer_candidate> _candidates; // rebuilt per pick; a member only to reuse its storage
    u64 _next_sequence = 0;
    cc::vector<pending_settle> _pending_settles; // finished streams, waiting on the copy fence

    u64 _current_window = 0; // next window index to submit; slot = index % num_staging_windows

    // One command list reused across every window; one allocator per window slot, reset when the window three back has completed.
    // Owned here rather than in the epoch-gated pool, since the copy queue does not observe epoch semantics.
    ComPtr<ID3D12GraphicsCommandList> _list;
    ComPtr<ID3D12CommandAllocator> _allocators[num_staging_windows];

    bool _window_open = false;
    isize _window_used = 0;       // bytes written into the open window so far
    u64 _open_max_wait_token = 0; // highest direct-queue token the open window's copies must wait for

    // Per-timeline highest completion value the open window finished; empty means it finished nothing, which is
    // also the question the acyclicity guard asks — see candidate_for.
    cc::vector<open_completion> _open_finished;

    // The one job with a still-pending reverse wait in the open window, if any.
    // While it is set the window admits nothing else, which is what keeps the acyclicity rule order-independent.
    cc::optional<u64> _open_risky_job;

    isize _window_async_bytes = 0; // what the open window has moved, per flavor, for the scheduler's deficit
    isize _window_stream_bytes = 0;
    double _window_opened_at = 0; // the one clock reading every candidate's age is measured against this window
};
} // namespace

cc::result<cc::unit> dx12_upload_async_system::initialize(isize window_bytes)
{
    CC_ASSERT(window_bytes > 0, "async upload staging window must be positive");
    window_bytes = round_window(window_bytes); // keep every window's base 512-aligned for texture copies

    D3D12_COMMAND_QUEUE_DESC copy_queue_desc = {};
    copy_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (HRESULT hr = _ctx._device->CreateCommandQueue(&copy_queue_desc, IID_PPV_ARGS(&_copy_queue)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateCommandQueue (async upload copy) failed");

    // UPLOAD heap, GENERIC_READ: the copy queue reads staged bytes from here via CopyBufferRegion.
    // Three windows back-to-back in one committed buffer, addressed by (window index % 3) * window_bytes.
    auto staging = create_mapped_ring_buffer(_ctx._device.Get(), D3D12_HEAP_TYPE_UPLOAD,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, window_bytes * num_staging_windows);
    CC_RETURN_IF_ERROR(staging);
    _staging = cc::move(staging.value().resource);
    _mapped = static_cast<byte*>(staging.value().mapped);
    _window_bytes = window_bytes;
    _desired_window_bytes.store(window_bytes, std::memory_order_relaxed); // no resize pending yet
    // The only other place that tells the scheduler is maybe_resize_staging, which a context that never resizes
    // never reaches — and without a window size its deficit bound is inert.
    _scheduler.set_window_bytes(window_bytes);

    if (HRESULT hr = _ctx._device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_window_fence)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateFence (async upload window) failed");

    _wait_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (_wait_event == nullptr)
        return cc::error("CreateEventW failed for the async upload wait event");

    _waker = std::make_shared<dx12_upload_waker>(*this);

    // The settle event is how a finished stream learns its copy has run without the actor blocking on the fence.
    // Registered after the waker exists, because the wait's callback dereferences it.
    _settle_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (_settle_event == nullptr)
        return cc::error("CreateEventW failed for the stream settle event");
    if (!RegisterWaitForSingleObject(&_settle_wait, _settle_event, &on_settle_fence_signaled, _waker.get(), INFINITE,
                                     WT_EXECUTEDEFAULT))
        return cc::error("RegisterWaitForSingleObject failed for the stream settle event");

    _actor = cc::make_and_start_threaded_actor<dx12_upload_async_actor>(*this);
    return cc::unit{};
}

void dx12_upload_async_system::upload_buffer(sg::raw_buffer_handle buffer, cc::pinned_data<byte const> data, isize offset)
{
    CC_ASSERT(buffer != nullptr, "async upload target buffer is null");
    auto const* const dst = dynamic_cast<dx12_buffer const*>(buffer.get());
    CC_ASSERT(dst != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(!dst->is_expired(), "async upload target is a transient buffer used past its epoch (expired)");
    CC_ASSERT(offset >= 0 && offset + data.size() <= dst->size_in_bytes(), "async upload range is out of the buffer's "
                                                                           "bounds");
    if (data.empty())
        return;
    CC_ASSERT(dst->_resource, "async upload target buffer has no storage");
    CC_ASSERT(dst->usage().has(sg::buffer_usage::copy_dst), "async upload target buffer must have "
                                                            "buffer_usage::copy_dst");
    CC_ASSERT(_mapped != nullptr, "async upload system used before initialization");

    // Reserve this upload's completion value on the BUFFER's own timeline and stamp it *before* enqueuing, so any
    // command list reading the buffer after this call already sees a value to wait on.
    // Per-destination is what keeps the value meaningful once jobs finish out of order — see dx12_completion_group.hh.
    CC_ASSERT(dst->_upload_group != nullptr, "an async upload target must have been created with copy_dst");
    u64 const value = dst->_upload_group->reserve();
    stamp_max(dst->_pending_async_upload_value, value);

    dx12_async_upload_job job;
    // Weak ref, not strong: a caller may drop its last handle before the actor stages this.
    // Resolving at stage time lets a released buffer skip the copy and its staging cost — see stage_job.
    // The storage stays alive independently until the copy fence reaches `value`, which is the lifetime gate.
    job.buffer_target = std::static_pointer_cast<dx12_buffer const>(cc::move(buffer)); // dst already dynamic_cast-verified
    job.dst_offset = offset;
    job.src = cc::move(data);
    job.completion = dx12_group_value{dst->_upload_group, value};
    // Reverse sync: defer this copy behind the last direct-queue list that used the buffer, so it never overwrites bytes an earlier-submitted list still reads.
    job.wait_token = sg::submission_token(dst->_last_used_submission_token.load(std::memory_order_acquire));
    _actor->enqueue_message(cc::move(job));
}

void dx12_upload_async_system::upload_texture(sg::raw_texture_handle texture,
                                              cc::pinned_data<byte const> data,
                                              sg::subresource_index const& subresource,
                                              sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "async upload target texture is null");
    auto const* const dst = dynamic_cast<dx12_texture const*>(texture.get());
    CC_ASSERT(dst != nullptr, "texture is not a dx12 texture");
    CC_ASSERT(!dst->is_expired(), "async upload target is a transient texture used past its epoch (expired)");
    CC_ASSERT(dst->_resource, "async upload target texture has no storage");
    CC_ASSERT(dst->usage().has(sg::texture_usage::copy_dst), "async upload target texture must have "
                                                             "texture_usage::copy_dst");
    CC_ASSERT(_mapped != nullptr, "async upload system used before initialization");

    // The region is already resolved (whole subresource / bounds-checked / empty→skipped) by the sg layer.
    dx12_texture_footprint const fp = compute_texture_footprint(dst->description(), subresource, region);
    CC_ASSERT(data.size() == fp.tight_size(), "async upload pixel data size does not match the copy region");

    // Reserve on the TEXTURE's own timeline and stamp before enqueuing — see upload_buffer.
    CC_ASSERT(dst->_upload_group != nullptr, "an async upload target must have been created with copy_dst");
    u64 const value = dst->_upload_group->reserve();
    stamp_max(dst->_pending_async_upload_value, value);

    dx12_async_upload_job job;
    job.texture_target = std::static_pointer_cast<dx12_texture const>(cc::move(texture));
    job.footprint = fp;
    job.is_texture = true;
    job.src = cc::move(data);
    job.completion = dx12_group_value{dst->_upload_group, value};
    // Reverse sync: defer the copy behind the last direct-queue list that used this texture.
    job.wait_token = sg::submission_token(dst->_last_used_submission_token.load(std::memory_order_acquire));
    _actor->enqueue_message(cc::move(job));
}

namespace
{
/// The control block every streaming upload shares, with its completion node and its promotion hook installed.
/// `on_promote` is what moves the reserved value onto the FORWARD stamp, which is the whole of what promotion means.
template <class ResourceT>
[[nodiscard]] std::shared_ptr<sg::impl::stream_control> make_stream_control(std::shared_ptr<ResourceT const> target,
                                                                            u64 value,
                                                                            i64 total_hint)
{
    auto control = std::make_shared<sg::impl::stream_control>();
    control->completion = cc::make_async_manual<cc::unit>();
    control->total_hint.store(total_hint, std::memory_order_relaxed);
    // Weak: promoting a transfer whose destination is already gone must not resurrect it.
    control->on_promote = [weak = std::weak_ptr<ResourceT const>(cc::move(target)), value]
    {
        if (auto const strong = weak.lock())
            stamp_max(strong->_pending_async_upload_value, value);
    };
    return control;
}
} // namespace

sg::stream_upload_handle dx12_upload_async_system::stream_buffer(sg::raw_buffer_handle buffer,
                                                                 cc::pinned_data<byte const> data,
                                                                 isize offset)
{
    // The resident form is one always-ready chunk, so it is the source seam's simplest case rather than a path of
    // its own — everything below it is shared.
    return stream_source_buffer(cc::move(buffer), sg::make_pinned_stream_source(cc::move(data)), offset);
}

sg::stream_upload_handle dx12_upload_async_system::stream_texture(sg::raw_texture_handle texture,
                                                                  cc::pinned_data<byte const> data,
                                                                  sg::subresource_index const& subresource,
                                                                  sg::texture_region const& region)
{
    return stream_source_texture(cc::move(texture), sg::make_pinned_stream_source(cc::move(data)), subresource, region);
}

sg::stream_upload_handle dx12_upload_async_system::stream_source_buffer(sg::raw_buffer_handle buffer,
                                                                        std::unique_ptr<sg::stream_source> source,
                                                                        isize offset)
{
    CC_ASSERT(buffer != nullptr, "stream upload target buffer is null");
    CC_ASSERT(source != nullptr, "stream upload source is null");
    auto const* const dst = dynamic_cast<dx12_buffer const*>(buffer.get());
    CC_ASSERT(dst != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(!dst->is_expired(), "stream upload target is a transient buffer used past its epoch (expired)");
    CC_ASSERT(dst->_resource, "stream upload target buffer has no storage");
    CC_ASSERT(dst->usage().has(sg::buffer_usage::copy_dst), "stream upload target buffer must have "
                                                            "buffer_usage::copy_dst");
    CC_ASSERT(_mapped != nullptr, "async upload system used before initialization");

    // Reserve a completion value and stamp the STREAMING slot only.
    // Deferred deletion reads it, so the storage outlives the copy; command-list access tracking does not, so no
    // later reader inherits a wait — which is the whole difference between the two tiers.
    CC_ASSERT(dst->_upload_group != nullptr, "a stream upload target must have been created with copy_dst");
    u64 const value = dst->_upload_group->reserve();
    stamp_max(dst->_pending_stream_copy_value, value);

    auto typed = std::static_pointer_cast<dx12_buffer const>(cc::move(buffer));
    auto control = make_stream_control(typed, value, source->total_size_hint());

    dx12_async_upload_job job;
    job.buffer_target = typed;
    job.dst_offset = offset;
    job.completion = dx12_group_value{dst->_upload_group, value};
    job.wait_token = sg::submission_token(dst->_last_used_submission_token.load(std::memory_order_acquire));
    job.stream = control;
    job.source = cc::move(source);
    _actor->enqueue_message(cc::move(job));

    return sg::stream_upload_handle(cc::move(control));
}

sg::stream_upload_handle dx12_upload_async_system::stream_source_texture(sg::raw_texture_handle texture,
                                                                         std::unique_ptr<sg::stream_source> source,
                                                                         sg::subresource_index const& subresource,
                                                                         sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "stream upload target texture is null");
    CC_ASSERT(source != nullptr, "stream upload source is null");
    auto const* const dst = dynamic_cast<dx12_texture const*>(texture.get());
    CC_ASSERT(dst != nullptr, "texture is not a dx12 texture");
    CC_ASSERT(!dst->is_expired(), "stream upload target is a transient texture used past its epoch (expired)");
    CC_ASSERT(dst->_resource, "stream upload target texture has no storage");
    CC_ASSERT(dst->usage().has(sg::texture_usage::copy_dst), "stream upload target texture must have "
                                                             "texture_usage::copy_dst");
    CC_ASSERT(_mapped != nullptr, "async upload system used before initialization");

    dx12_texture_footprint const fp = compute_texture_footprint(dst->description(), subresource, region);

    CC_ASSERT(dst->_upload_group != nullptr, "a stream upload target must have been created with copy_dst");
    u64 const value = dst->_upload_group->reserve();
    stamp_max(dst->_pending_stream_copy_value, value);

    auto typed = std::static_pointer_cast<dx12_texture const>(cc::move(texture));
    auto control = make_stream_control(typed, value, source->total_size_hint());

    dx12_async_upload_job job;
    job.texture_target = typed;
    job.footprint = fp;
    job.is_texture = true;
    job.completion = dx12_group_value{dst->_upload_group, value};
    job.wait_token = sg::submission_token(dst->_last_used_submission_token.load(std::memory_order_acquire));
    job.stream = control;
    job.source = cc::move(source);
    _actor->enqueue_message(cc::move(job));

    return sg::stream_upload_handle(cc::move(control));
}

void dx12_upload_async_system::wake_actor()
{
    if (_actor)
        _actor->enqueue_message(dx12_transfer_wake{});
}

void dx12_upload_waker::wake()
{
    _target.lock(
        [](target& t)
        {
            if (t.system != nullptr)
                t.system->wake_actor();
        });
}

void dx12_upload_async_system::set_window_bytes(isize bytes)
{
    CC_ASSERT(bytes > 0, "async upload staging window must be positive");
    // Record the request; the copy actor adopts it at the top of its next process cycle (before staging).
    _desired_window_bytes.store(bytes, std::memory_order_release);
}

void dx12_upload_async_system::shutdown()
{
    // Unregister before anything else: INVALID_HANDLE_VALUE waits out any callback already running, so past this
    // line nothing can dereference the waker the wait was given.
    if (_settle_wait != nullptr)
    {
        UnregisterWaitEx(_settle_wait, INVALID_HANDLE_VALUE);
        _settle_wait = nullptr;
    }

    // Detach next: a source may hand its waker to a thread that fires long after the transfer ended, and after
    // this returns such a wake finds nothing rather than an actor being destroyed under it.
    if (_waker)
    {
        _waker->detach();
        _waker = nullptr;
    }
    if (_actor)
    {
        _actor->shutdown(); // drains queued copies; on_thread_shutdown waits for the copy queue to idle
        _actor = nullptr;
    }
    if (_staging)
    {
        _staging->Unmap(0, nullptr);
        _staging.Reset();
    }
    _mapped = nullptr;
    _window_fence.Reset();
    _copy_queue.Reset();
    if (_wait_event)
    {
        CloseHandle(_wait_event);
        _wait_event = nullptr;
    }
    if (_settle_event)
    {
        CloseHandle(_settle_event);
        _settle_event = nullptr;
    }
}
} // namespace sg::backend::dx12
