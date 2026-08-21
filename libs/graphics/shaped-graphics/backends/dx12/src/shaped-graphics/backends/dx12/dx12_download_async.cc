// dx12_download_async_system: the copy actor behind dx12_download_async.hh — window packing, submission, and the in-flight drain.
// The shape and the two fences are on the class doc there.
// Why the drain rule and the acyclicity guard are load-bearing: libs/graphics/shaped-graphics/docs/concepts/download.async.md.

#include <clean-core/common/time.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_download_async.hh>
#include <shaped-graphics/backends/dx12/dx12_resource_download.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
#include <shaped-graphics/transfer/impl/transfer_scheduler.hh>

namespace sg::backend::dx12
{
namespace
{
// Triple-buffered staging: one window read by the GPU, one just submitted, one being filled by the CPU.
// Fewer than three reintroduces a sync bubble; more only adds staging memory.
constexpr int num_staging_windows = 3;

// Window sizes round up to the texture placement alignment (512), so each window base is 512-aligned.
// A texture readback's placed footprint must start there; buffer readbacks are unaffected by the rounding.
[[nodiscard]] isize round_window(isize bytes)
{
    return (bytes + texture_placement_alignment - 1) / texture_placement_alignment * texture_placement_alignment;
}

// One chunk's CPU memcpy out of the readback staging buffer, deferred until its window's GPU read has run.
// `source_keepalive` holds the read source alive until then: the read is recorded but not yet executed when the job leaves the inbox.
struct download_mem_job
{
    cc::unique_function<void()> deferred_cpu_copy;
    std::weak_ptr<void const> pin;                // future's pin; expired == caller cancelled the copy
    cc::shared_async<cc::unit> completion;        // set only on a job's last chunk; settles the future
    std::shared_ptr<void const> source_keepalive; // holds the source (buffer or texture) alive across the read
    std::shared_ptr<dx12_download_sink> sink;     // set for a sink-driven read; its `failed` decides how this settles
};

// A submitted-but-not-yet-drained window.
// Its GPU read completes when the window fence reaches window_index + 1; its memcpies then run in order and their futures settle.
struct inflight_window
{
    u64 window_index = 0;
    cc::vector<download_mem_job> mem_jobs;
};

/// The highest completion value the open window finished on one timeline; mirrors the upload actor's twin.
struct open_completion
{
    dx12_completion_group_handle group;
    u64 value = 0;
};

/// A readback the actor has resolved and is packing.
///
/// The packer carries the per-job cursor, so a read survives across windows AND across process cycles.
/// That is what lets the actor pick a different job between chunks rather than draining each one to completion
/// before it looks at the next, which is the whole basis of out-of-order selection.
struct active_download
{
    dx12_async_download_job job;
    std::unique_ptr<dx12_resource_download> packer;
    std::shared_ptr<void const> keepalive; // the read source, held across the deferred CPU copies
    u64 sequence = 0;                      // actor-assigned submission order
    u64 family = 0;                        // source resource: same-family reads stay in sequence order

    // When the actor took this job on, for aging.
    // Read once per window rather than per candidate: aging changes an effective priority continuously, but the
    // scheduler only acts on it when a window opens, so a finer reading would cost clock reads to no effect.
    double admitted_at = 0;
};

/// The async-download copy actor: one thread that packs readbacks into staging windows, submits them, and drains completed windows into their destinations.
/// All window / job / command-list state lives here and is touched only on the actor thread, so it needs no locks.
/// It reaches the shared, immutable-after-init fields (staging buffer, fences, queue) via _sys.
class dx12_download_async_actor final : public cc::threaded_actor_impl<dx12_async_download_job>
{
public:
    explicit dx12_download_async_actor(dx12_download_async_system& sys) : _sys(sys) {}

    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "sg-dx12-download-async"; }

protected:
    void on_message(dx12_async_download_job job) override { _pending.push_back(cc::move(job)); }

    bool on_process() override
    {
        maybe_resize_staging(); // adopt a pending set_window_bytes now, while no window is open + FIFO drained

        admit_pending();
        pack_until_stalled();
        submit_window(); // flush the final partial window so its reads run
        drain_all();     // wait + memcpy every in-flight window so all futures are ready before we sleep
        return false;    // everything drained + delivered; sleep until the next message
    }

    void on_thread_shutdown() override
    {
        // Flush anything still buffered, then drain every window — that waits out the copy queue, so the staging buffer and command list/allocators are safe to release.
        admit_pending();
        pack_until_stalled();

        // Anything still active at shutdown is cancelled rather than abandoned: a future or handle waiting on a node
        // nobody will ever push would hang the very teardown trying to finish.
        for (auto& a : _active)
            fold_cancelled_completion(a.job);
        _active.clear();

        submit_window();
        drain_all();
    }

private:
    // A cancelled read, whose future's pin expired: skip the read entirely — no copy, no forward wait, and no read-after-write hazard to order.
    // But STILL fold the completion value so the download fence reaches it; a later writer stamped with it must not hang.
    // An empty window still submits and signals, keeping the fence monotonic and gap-free.
    void fold_cancelled_completion(dx12_async_download_job& job)
    {
        // The future itself is gone, but a completion() handed out earlier can outlive it — and a manual node nobody
        // pushes parks its dependents forever, so cancellation has to be said out loud.
        if (job.completion)
            job.completion->push_error(cc::async_error::make_cancelled());

        fold_completion_value(job);
    }

    // Folds a read's completion value into the open window, on its source's own timeline.
    // Mandatory on every exit path: a promoted read hands that value to a later writer, and a value the fence never
    // reaches would hang it.
    void fold_completion_value(dx12_async_download_job const& job)
    {
        if (!job.completion_value.is_pending())
            return;
        ensure_open_window();
        for (auto& f : _open_finished)
            if (f.group == job.completion_value.group)
            {
                if (job.completion_value.value > f.value)
                    f.value = job.completion_value.value;
                return;
            }
        _open_finished.push_back(open_completion{job.completion_value.group, job.completion_value.value});
    }

    // Resolves each arriving readback's source and gives it a resumable packer.
    // The source is held strong for the job's whole lifetime, so its storage survives the read without a deferred-deletion gate.
    void admit_pending()
    {
        // One reading for the whole batch — see the upload actor's twin.
        double const admitted_at = cc::current_time_steady_secs();
        for (auto& job : _pending)
        {
            // A sink-driven read has no resident destination, so it has no pin either — cancellation comes from
            // its handle instead, which reap_cancelled picks up.
            if (job.sink == nullptr && job.pin.expired())
            {
                fold_cancelled_completion(job);
                continue;
            }

            active_download a;
            if (job.is_texture)
            {
                CC_ASSERT(round_window(job.footprint.padded_pitch) <= _sys._window_bytes, "a single texture row "
                                                                                          "exceeds one staging "
                                                                                          "window");
                a.family = u64(reinterpret_cast<u64>(job.texture_source->_resource.Get()));
                if (job.sink)
                    a.packer = std::make_unique<dx12_texture_download>(job.texture_source->_resource.Get(),
                                                                       job.footprint, job.sink);
                else
                    a.packer = std::make_unique<dx12_texture_download>(job.texture_source->_resource.Get(),
                                                                       job.footprint, job.dst);
                a.keepalive = job.texture_source;
            }
            else
            {
                a.family = u64(reinterpret_cast<u64>(job.buffer_source->_resource.Get()));
                if (job.sink)
                    a.packer = std::make_unique<dx12_buffer_download>(job.buffer_source->_resource.Get(),
                                                                      job.src_offset, job.size, job.sink);
                else
                    a.packer = std::make_unique<dx12_buffer_download>(job.buffer_source->_resource.Get(),
                                                                      job.src_offset, job.dst);
                a.keepalive = job.buffer_source;
            }
            a.sequence = _next_sequence++;
            a.admitted_at = admitted_at;
            a.job = cc::move(job);
            _active.push_back(cc::move(a));
        }
        _pending.clear();
    }

    // Drops every active streaming read whose handle has cancelled it.
    // Cancellation is a flag rather than queue surgery: the read stops being picked and is reaped when the actor
    // next looks.
    // Chunks already recorded still run, since their staging bytes are committed.
    void reap_cancelled()
    {
        for (isize i = _active.size() - 1; i >= 0; --i)
        {
            auto& a = _active[i];
            if (a.job.stream == nullptr || !a.job.stream->cancelled.load(std::memory_order_relaxed))
                continue;
            fold_cancelled_completion(a.job); // settles the shared node, so the future fails rather than hanging
            _active.remove_from_to(i, i + 1);
        }
    }

    // How the scheduler sees one active readback against the currently open window.
    //
    // A window issues its cross-queue waits once, hoisted ahead of its reads (submit_window), so it must never both
    // promise a completion V and carry a wait that could depend on V — the copy-actor deadlock.
    // There are two such waits per read: the forward direct-queue token, and the upload completion value.
    // The async upload a read waits on may itself reverse-wait on a direct writer that waits on this window's V.
    //
    // Two eligibility rules keep that true INDEPENDENT of the order reads are packed in:
    //  - a read with either wait still pending may not join a window that has already finished a read;
    //  - once such a read IS in the open window, no other read may join, so no other read's completion can land
    //    beside its waits.
    // A read's own completion is safe beside its own waits, since both were read before its value was reserved.
    [[nodiscard]] sg::impl::transfer_candidate candidate_for(active_download const& a) const
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

        if (has_pending_wait(a.job) && !_open_finished.empty())
            c.eligible = false;
        if (_open_risky_job.has_value() && _open_risky_job.value() != a.sequence)
            c.eligible = false;
        return c;
    }

    [[nodiscard]] bool has_pending_wait(dx12_async_download_job const& job) const
    {
        bool const forward_pending = u64(job.wait_token) > _sys._ctx._submission_fence->GetCompletedValue();
        return forward_pending || !job.upload_wait_value.has_reached();
    }

    // Picks and packs one chunk at a time until nothing can make progress in any window.
    // A read that cannot go is passed over rather than blocking the queue behind it.
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
                // Nothing fits THIS window; closing it clears both eligibility blocks.
                // "Pristine" must mean no folded completion either, not just no bytes: a batch whose first reads
                // were all cancelled folds their values into an otherwise empty window, and that alone blocks every
                // pending-wait read.
                // Stopping there would leave those reads unstaged and their completion values never signaled.
                if (_window_used == 0 && _open_finished.empty())
                    break;
                submit_window();
                continue;
            }

            isize const index = pick.value();
            if (!pack_chunk(_active[index]))
            {
                CC_ASSERT(_window_used > 0, "an empty staging window could not fit a single chunk");
                submit_window(); // window tail too small for the next aligned texture row → roll to a fresh one
                continue;
            }

            if (_active[index].packer->is_finished())
                _active.remove_from_to(index, index + 1);

            // Independent of whether that finished the read: one ending exactly on the window boundary still leaves
            // a full window, and the next pick would be handed a zero-byte allocation.
            if (_window_used == _sys._window_bytes)
                submit_window();
        }

        CC_ASSERT(_active.empty(), "async download actor stalled with reads still unstaged");
    }

    // Records one chunk of `a` into the open window and defers its CPU memcpy to drain.
    // False when the window tail cannot fit the read's next aligned chunk; the caller rolls to a fresh window.
    [[nodiscard]] bool pack_chunk(active_download& a)
    {
        isize const avail = _sys._window_bytes - _window_used;
        isize const base = isize(_current_window % u64(num_staging_windows)) * _sys._window_bytes;
        dx12_download_allocation const alloc = {_sys._staging.Get(), _sys._mapped, base + _window_used, avail};

        dx12_pending_copy chunk = a.packer->execute_next_job(*_list.Get(), alloc);
        if (chunk.bytes == 0)
            return false;
        _window_used += chunk.bytes;
        if (a.job.stream)
        {
            _window_stream_bytes += chunk.bytes;
            a.job.stream->bytes_done.fetch_add(chunk.bytes, std::memory_order_relaxed);
        }
        else
            _window_async_bytes += chunk.bytes;

        // This chunk reads the source, so its window must first wait for the last direct-queue list that used it, and for any pending async upload to it.
        // Max over the window; both fences are monotonic.
        if (u64(a.job.wait_token) > _open_max_wait_token)
            _open_max_wait_token = u64(a.job.wait_token);
        fold_open_upload_wait(a.job.upload_wait_value);
        if (has_pending_wait(a.job))
            _open_risky_job = a.sequence; // the window is now dedicated to it — see candidate_for

        // The window holding the read's last byte is the one whose completion satisfies a later writer's reverse wait.
        // Max PER TIMELINE, which is exact because the scheduler keeps same-source reads in sequence order.
        bool const last = a.packer->is_finished();
        if (last)
            fold_completion_value(a.job);

        // Defer the CPU memcpy until this window's GPU read completes, at drain.
        // Only the last chunk settles the future: windows drain in order, so every earlier chunk is copied by then.
        _open_mem_jobs.push_back(download_mem_job{cc::move(chunk.deferred_cpu_copy), a.job.pin,
                                                  last ? a.job.completion : cc::shared_async<cc::unit>(), a.keepalive,
                                                  a.job.sink});
        return true;
    }

    // Ensures a window is open with room to write.
    // One command list is reused across all windows; each of the three window slots has its own allocator.
    // Reusing a slot first fully drains the window three submissions back, so its staging memory, its allocator's GPU work AND its CPU memcpies are done before the slot is overwritten.
    void ensure_open_window()
    {
        if (_window_open)
            return;

        int const slot = int(_current_window % u64(num_staging_windows));
        if (_current_window >= u64(num_staging_windows))
            drain_until(_current_window - u64(num_staging_windows));

        if (_allocators[slot] == nullptr) // first use of this slot: fresh allocator, ready to record
        {
            HRESULT const ha = _sys._ctx._device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                                                                         IID_PPV_ARGS(&_allocators[slot]));
            CC_ASSERT(SUCCEEDED(ha), "ID3D12Device::CreateCommandAllocator (copy) failed");
        }
        else // reuse: the window that last used this allocator has drained (above)
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
        _open_upload_waits.clear();
        _open_risky_job = {};
        _window_async_bytes = 0;
        _window_stream_bytes = 0;
        _window_opened_at = cc::current_time_steady_secs();
        _open_mem_jobs.clear();
        _sys._scheduler.begin_window();
        _window_open = true;
    }

    // Closes + submits the open window: hoists its forward-sync waits, executes it, then signals the window fence — its slot is reusable once drained.
    // If the window finished any read it also signals the download completion fence up to the highest finished value.
    // The window's memcpies are deferred, pushed to the in-flight FIFO to be drained later.
    // No-op when no window is open.
    void submit_window()
    {
        if (!_window_open)
            return;

        HRESULT const hc = _list->Close();
        CC_ASSERT(SUCCEEDED(hc), "ID3D12GraphicsCommandList::Close failed");

        // Forward sync: hold the copy queue until every direct-queue list that used this window's sources has finished.
        // The read then sees committed bytes and never races an in-flight writer.
        // Over-waiting on a higher (monotonic) token is safe, and an already-completed token returns at once.
        if (_open_max_wait_token > 0)
            _sys._copy_queue->Wait(_sys._ctx._submission_fence.Get(), _open_max_wait_token);

        // Forward cross-queue sync vs a pending async upload: hold this queue until each source's upload timeline
        // reaches the value this window's reads must observe.
        // One Wait per timeline, because an upload value only means anything against the resource that reserved it.
        // A read then never races an in-flight upload to the same buffer.
        // The acyclicity guard above keeps these hoisted waits from ever preceding a read whose completion the upload transitively depends on.
        for (auto const& w : _open_upload_waits)
            _sys._copy_queue->Wait(w.group->fence.Get(), w.value);

        ID3D12CommandList* lists[] = {_list.Get()};
        _sys._copy_queue->ExecuteCommandLists(1, lists);

        // Window fence is 1-based: window i completing signals i+1.
        // The fence starts at 0, so a 0-based value for window 0 would read as "not yet started" and wait_for_window(0) would never block.
        // wait_for_window applies the same +1, so callers pass window indices.
        HRESULT const hs = _sys._copy_queue->Signal(_sys._window_fence.Get(), _current_window + 1);
        CC_ASSERT(SUCCEEDED(hs), "ID3D12CommandQueue::Signal (download window) failed");

        // One Signal per timeline this window finished a read on, each monotonic on its own fence.
        // Windows carrying only a mid-read chunk finish nothing and signal nothing.
        for (auto const& f : _open_finished)
        {
            if (f.value <= f.group->last_signaled)
                continue;
            HRESULT const hcf = _sys._copy_queue->Signal(f.group->fence.Get(), f.value);
            CC_ASSERT(SUCCEEDED(hcf), "ID3D12CommandQueue::Signal (download completion) failed");
            f.group->last_signaled = f.value;
        }

        _inflight.push_back(inflight_window{_current_window, cc::move(_open_mem_jobs)});
        _open_mem_jobs = {};
        _sys._scheduler.on_window_submitted(_window_async_bytes, _window_stream_bytes);
        _window_open = false;
        ++_current_window;
    }

    // Drains every in-flight window whose index is <= `last_index_inclusive` (FIFO, oldest first).
    void drain_until(u64 last_index_inclusive)
    {
        while (!_inflight.empty() && _inflight[0].window_index <= last_index_inclusive)
            drain_front();
    }

    // Drains every in-flight window — waits out the copy queue and delivers all pending memcpies.
    void drain_all()
    {
        while (!_inflight.empty())
            drain_front();
    }

    // Drains the oldest in-flight window: waits for its GPU read, runs its memcpies in order (skipping any whose future was dropped), and settles each last-chunk future.
    void drain_front()
    {
        inflight_window w = cc::move(_inflight[0]);
        _inflight.remove_from_to(0, 1);

        wait_for_window(w.window_index);
        for (auto& mj : w.mem_jobs)
        {
            // A sink-driven read has no pin to consult: its sink is the destination, and it lives as long as the
            // transfer does.
            bool const wanted = mj.sink != nullptr || mj.pin.lock() != nullptr;
            if (wanted)
                mj.deferred_cpu_copy();
            if (mj.completion)
            {
                // Settling is mandatory on every branch — a manual node nobody ever pushes parks its dependents for
                // the process's lifetime.
                if (!wanted) // the destination was dropped mid-flight: a cancellation, not a delivery
                    mj.completion->push_error(cc::async_error::make_cancelled());
                else if (mj.sink && mj.sink->failed.load(std::memory_order_relaxed))
                    mj.completion->push_error(cc::async_error::make_error(cc::any_error("stream sink rejected the "
                                                                                        "downloaded bytes")));
                else
                    mj.completion->push_value(cc::unit{});
            }
        }
    }

    // Adopts a pending set_window_bytes when one differs from the current size.
    // Called at the top of a process cycle, when no window is open.
    // Submits any open window and drains every in-flight one — each memcpys out of the old staging buffer, so it cannot be freed until they are all done.
    // Then rebuilds staging at the new size; the per-slot allocators and the reused command list survive.
    void maybe_resize_staging()
    {
        isize const desired = round_window(_sys._desired_window_bytes.load(std::memory_order_acquire));
        if (desired == _sys._window_bytes)
            return;
        CC_ASSERT(desired > 0, "async download staging window must be positive");

        submit_window();
        drain_all();

        // Build the new staging buffer before releasing the old, so a failed allocation leaves the current one intact.
        // The resize is then simply not applied, and the next cycle retries.
        auto ring = create_mapped_ring_buffer(_sys._ctx._device.Get(), D3D12_HEAP_TYPE_READBACK,
                                              D3D12_RESOURCE_STATE_COPY_DEST, desired * num_staging_windows);
        CC_ASSERT(ring.has_value(), "async download staging resize failed to allocate");

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

    dx12_download_async_system& _sys;

    cc::vector<dx12_async_download_job> _pending;         // received since the last cycle, resolved in admit_pending
    cc::vector<active_download> _active;                  // resolved and mid-pack, until the last chunk is recorded
    cc::vector<sg::impl::transfer_candidate> _candidates; // rebuilt per pick; a member only to reuse its storage
    u64 _next_sequence = 0;

    u64 _current_window = 0; // next window index to submit; slot = index % num_staging_windows

    // One command list reused across every window; one allocator per window slot, reset when the window three back has drained.
    // Owned here rather than in the epoch-gated pool, since the copy queue does not observe epoch semantics.
    ComPtr<ID3D12GraphicsCommandList> _list;
    ComPtr<ID3D12CommandAllocator> _allocators[num_staging_windows];

    bool _window_open = false;
    isize _window_used = 0;                      // bytes read into the open window so far
    u64 _open_max_wait_token = 0;                // highest direct-queue token the open window's reads must wait for
    cc::vector<download_mem_job> _open_mem_jobs; // memcpies accumulated for the open window

    // Per-timeline highest completion value the open window finished; empty means it finished nothing, which is also
    // what the acyclicity guard asks.
    cc::vector<open_completion> _open_finished;

    // The upload timelines the open window's reads must observe before they run, at the highest value each owes.
    cc::vector<dx12_group_value> _open_upload_waits;

    // Raises the entry sharing `w`'s timeline, or adds one; unrelated timelines cannot be merged into a single max.
    void fold_open_upload_wait(dx12_group_value const& w)
    {
        if (!w.is_pending())
            return;
        for (auto& existing : _open_upload_waits)
            if (existing.try_raise_to(w))
                return;
        _open_upload_waits.push_back(w);
    }

    // The one read with a still-pending cross-queue wait in the open window, if any.
    // While it is set the window admits nothing else, which is what keeps the acyclicity rule order-independent.
    cc::optional<u64> _open_risky_job;

    isize _window_async_bytes = 0; // what the open window has moved, per flavor, for the scheduler's deficit
    isize _window_stream_bytes = 0;
    double _window_opened_at = 0; // the one clock reading every candidate's age is measured against this window

    cc::vector<inflight_window> _inflight; // submitted, not-yet-drained windows (FIFO, oldest at the front)
};
} // namespace

cc::result<cc::unit> dx12_download_async_system::initialize(isize window_bytes)
{
    CC_ASSERT(window_bytes > 0, "async download staging window must be positive");
    window_bytes = round_window(window_bytes); // keep every window's base 512-aligned for texture readbacks

    D3D12_COMMAND_QUEUE_DESC copy_queue_desc = {};
    copy_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (HRESULT hr = _ctx._device->CreateCommandQueue(&copy_queue_desc, IID_PPV_ARGS(&_copy_queue)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateCommandQueue (async download copy) failed");

    // READBACK heap, COPY_DEST: the copy queue writes read bytes here via CopyBufferRegion, the actor reads them back out on the CPU.
    // Three windows back-to-back, addressed by (window index % 3) * window_bytes.
    auto staging = create_mapped_ring_buffer(_ctx._device.Get(), D3D12_HEAP_TYPE_READBACK,
                                             D3D12_RESOURCE_STATE_COPY_DEST, window_bytes * num_staging_windows);
    CC_RETURN_IF_ERROR(staging);
    _staging = cc::move(staging.value().resource);
    _mapped = static_cast<byte*>(staging.value().mapped);
    _window_bytes = window_bytes;
    _desired_window_bytes.store(window_bytes, std::memory_order_relaxed); // no resize pending yet
    // See the upload twin: the resize path is not the only path, so the bound has to be established here.
    _scheduler.set_window_bytes(window_bytes);

    if (HRESULT hr = _ctx._device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_window_fence)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateFence (async download window) failed");

    _wait_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (_wait_event == nullptr)
        return cc::error("CreateEventW failed for the async download wait event");

    _actor = cc::make_and_start_threaded_actor<dx12_download_async_actor>(*this);
    return cc::unit{};
}

sg::bytes_future dx12_download_async_system::download_buffer(sg::raw_buffer_handle buffer, isize offset, isize size)
{
    CC_ASSERT(buffer != nullptr, "async download source buffer is null");
    auto const* const src = dynamic_cast<dx12_buffer const*>(buffer.get());
    CC_ASSERT(src != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(!src->is_expired(), "async download source is a transient buffer used past its epoch (expired)");
    CC_ASSERT(size >= 0, "async download size must be non-negative");
    CC_ASSERT(offset >= 0 && offset + size <= src->size_in_bytes(), "async download range is out of the buffer's "
                                                                    "bounds");

    // zero-size read: already-ready, empty future (no staging, no actor work).
    if (size == 0)
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_ready_completion());

    CC_ASSERT(src->_resource, "async download source buffer has no storage");
    CC_ASSERT(src->usage().has(sg::buffer_usage::copy_src), "async download source buffer must have "
                                                            "buffer_usage::copy_src");
    CC_ASSERT(_mapped != nullptr, "async download system used before initialization");

    // Forward cross-queue sync vs a pending async UPLOAD to the same buffer: the read must observe it.
    // Upload and download own independent copy queues, so the read's window waits on the upload completion fence for this value.
    // That wait is issued on the download queue in submit_window — a clean GPU wait, no CPU stall.
    u64 const upload_wait = src->_pending_async_upload_value.load(std::memory_order_acquire);

    // Destination the read bytes land in; the pinned_data keeps it alive until the copy runs (or cancels).
    auto dst = cc::pinned_data<byte>::create_uninitialized(size);
    cc::span<byte> const dst_span = dst.span();
    auto completion = cc::make_async_manual<cc::unit>();

    // Reserve this read's completion value and stamp the buffer *before* enqueuing, so a later direct-queue list that writes the buffer already sees a value to wait on.
    CC_ASSERT(src->_download_group != nullptr, "a download source must have been created with copy_src");
    u64 const value = src->_download_group->reserve();
    u64 prev = src->_pending_async_download_value.load(std::memory_order_relaxed);
    while (prev < value
           && !src->_pending_async_download_value.compare_exchange_weak(prev, value, std::memory_order_release,
                                                                        std::memory_order_relaxed))
    {
        // CAS retries; `prev` is refreshed with the current value each time.
    }

    dx12_async_download_job job;
    // Held strong for the job's whole lifetime, so the source storage survives the copy-queue read.
    // `src` was already dynamic_cast-verified above.
    job.buffer_source = std::static_pointer_cast<dx12_buffer const>(cc::move(buffer));
    job.src_offset = offset;
    job.size = size;
    job.dst = dst_span;
    job.pin = std::weak_ptr<void const>(dst.pin()); // weak: dropping the future cancels the copy
    job.completion = completion;
    job.completion_value = dx12_group_value{src->_download_group, value};
    // Forward sync: defer the read behind the last direct-queue list that used the buffer, so it reads committed bytes and never races an earlier-submitted writer.
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(std::memory_order_acquire));
    // A null upload group means the source can never have been an upload target, so nothing to observe.
    job.upload_wait_value = dx12_group_value{src->_upload_group, upload_wait};
    _actor->enqueue_message(cc::move(job));

    return sg::bytes_future(cc::move(dst), cc::move(completion));
}

sg::bytes_future dx12_download_async_system::download_texture(sg::raw_texture_handle texture,
                                                              sg::subresource_index const& subresource,
                                                              sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "async download source texture is null");
    auto const* const src = dynamic_cast<dx12_texture const*>(texture.get());
    CC_ASSERT(src != nullptr, "texture is not a dx12 texture");
    CC_ASSERT(!src->is_expired(), "async download source is a transient texture used past its epoch (expired)");
    CC_ASSERT(src->_resource, "async download source texture has no storage");
    CC_ASSERT(src->usage().has(sg::texture_usage::copy_src), "async download source texture must have "
                                                             "texture_usage::copy_src");
    CC_ASSERT(_mapped != nullptr, "async download system used before initialization");

    // The region is already resolved (whole subresource / bounds-checked / empty→skipped) by the sg layer.
    dx12_texture_footprint const fp = compute_texture_footprint(src->description(), subresource, region);

    u64 const upload_wait = src->_pending_async_upload_value.load(std::memory_order_acquire);

    auto dst = cc::pinned_data<byte>::create_uninitialized(fp.tight_size());
    cc::span<byte> const dst_span = dst.span();
    auto completion = cc::make_async_manual<cc::unit>();

    CC_ASSERT(src->_download_group != nullptr, "a download source must have been created with copy_src");
    u64 const value = src->_download_group->reserve();
    u64 prev = src->_pending_async_download_value.load(std::memory_order_relaxed);
    while (prev < value
           && !src->_pending_async_download_value.compare_exchange_weak(prev, value, std::memory_order_release,
                                                                        std::memory_order_relaxed))
    {
        // CAS retries; `prev` refreshed each time.
    }

    dx12_async_download_job job;
    job.texture_source = std::static_pointer_cast<dx12_texture const>(cc::move(texture));
    job.footprint = fp;
    job.is_texture = true;
    job.dst = dst_span;
    job.pin = std::weak_ptr<void const>(dst.pin());
    job.completion = completion;
    job.completion_value = dx12_group_value{src->_download_group, value};
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(std::memory_order_acquire));
    job.upload_wait_value = dx12_group_value{src->_upload_group, upload_wait};
    _actor->enqueue_message(cc::move(job));

    return sg::bytes_future(cc::move(dst), cc::move(completion));
}

namespace
{
/// The control block every streaming readback shares, with its completion node and its promotion hook installed.
///
/// Promotion means the same thing here as it does for an upload, with the direction flipped: the reserved value moves
/// onto the resource's REVERSE stamp, so a command list that WRITES the source after the call waits for the read.
/// Nothing stamps it up front — a streamed extent costs a later writer nothing until someone asks for that.
template <class ResourceT>
[[nodiscard]] std::shared_ptr<sg::impl::stream_control> make_stream_control(std::shared_ptr<ResourceT const> source,
                                                                            u64 value,
                                                                            i64 total_hint)
{
    auto control = std::make_shared<sg::impl::stream_control>();
    control->completion = cc::make_async_manual<cc::unit>();
    control->total_hint.store(total_hint, std::memory_order_relaxed);
    // Weak: promoting a read whose source is already gone must not resurrect it.
    control->on_promote = [weak = std::weak_ptr<ResourceT const>(cc::move(source)), value]
    {
        if (auto const strong = weak.lock())
        {
            u64 prev = strong->_pending_async_download_value.load(std::memory_order_relaxed);
            while (prev < value
                   && !strong->_pending_async_download_value.compare_exchange_weak(
                       prev, value, std::memory_order_release, std::memory_order_relaxed))
            {
                // CAS retries; `prev` is refreshed with the current value each time.
            }
        }
    };
    return control;
}
} // namespace

sg::stream_download_handle dx12_download_async_system::stream_buffer(sg::raw_buffer_handle buffer, isize offset, isize size)
{
    CC_ASSERT(buffer != nullptr, "stream download source buffer is null");
    auto const* const src = dynamic_cast<dx12_buffer const*>(buffer.get());
    CC_ASSERT(src != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(!src->is_expired(), "stream download source is a transient buffer used past its epoch (expired)");
    CC_ASSERT(src->_resource, "stream download source buffer has no storage");
    CC_ASSERT(src->usage().has(sg::buffer_usage::copy_src), "stream download source buffer must have "
                                                            "buffer_usage::copy_src");
    CC_ASSERT(_mapped != nullptr, "async download system used before initialization");

    // A value is RESERVED but not stamped: nothing waits on a streamed read until promote_to_async says so.
    // It is still folded into the window when the read finishes, so the fence reaches it — otherwise a promotion
    // would hand a later writer a value the fence never gets to, and hang it.
    CC_ASSERT(src->_download_group != nullptr, "a download source must have been created with copy_src");
    u64 const value = src->_download_group->reserve();

    auto dst = cc::pinned_data<byte>::create_uninitialized(size);
    cc::span<byte> const dst_span = dst.span();
    auto typed = std::static_pointer_cast<dx12_buffer const>(cc::move(buffer));
    auto control = make_stream_control(typed, value, size);

    dx12_async_download_job job;
    job.buffer_source = typed;
    job.src_offset = offset;
    job.size = size;
    job.dst = dst_span;
    job.pin = std::weak_ptr<void const>(dst.pin());
    job.stream = control;
    // One node serves both the future and the handle, so the drain path already settles them together.
    job.completion = control->completion;
    job.completion_value = dx12_group_value{src->_download_group, value};
    // The forward waits stay, since reading bytes an earlier list has not finished writing would just be wrong.
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(std::memory_order_acquire));
    job.upload_wait_value
        = dx12_group_value{src->_upload_group, src->_pending_async_upload_value.load(std::memory_order_acquire)};
    _actor->enqueue_message(cc::move(job));

    return sg::stream_download_handle(cc::move(control), sg::bytes_future(cc::move(dst), control->completion));
}

sg::stream_download_handle dx12_download_async_system::stream_texture(sg::raw_texture_handle texture,
                                                                      sg::subresource_index const& subresource,
                                                                      sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "stream download source texture is null");
    auto const* const src = dynamic_cast<dx12_texture const*>(texture.get());
    CC_ASSERT(src != nullptr, "texture is not a dx12 texture");
    CC_ASSERT(!src->is_expired(), "stream download source is a transient texture used past its epoch (expired)");
    CC_ASSERT(src->_resource, "stream download source texture has no storage");
    CC_ASSERT(src->usage().has(sg::texture_usage::copy_src), "stream download source texture must have "
                                                             "texture_usage::copy_src");
    CC_ASSERT(_mapped != nullptr, "async download system used before initialization");

    dx12_texture_footprint const fp = compute_texture_footprint(src->description(), subresource, region);
    CC_ASSERT(src->_download_group != nullptr, "a download source must have been created with copy_src");
    u64 const value = src->_download_group->reserve();

    auto dst = cc::pinned_data<byte>::create_uninitialized(fp.tight_size());
    cc::span<byte> const dst_span = dst.span();
    auto typed = std::static_pointer_cast<dx12_texture const>(cc::move(texture));
    auto control = make_stream_control(typed, value, fp.tight_size());

    dx12_async_download_job job;
    job.texture_source = typed;
    job.footprint = fp;
    job.is_texture = true;
    job.dst = dst_span;
    job.pin = std::weak_ptr<void const>(dst.pin());
    job.stream = control;
    job.completion = control->completion;
    job.completion_value = dx12_group_value{src->_download_group, value};
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(std::memory_order_acquire));
    job.upload_wait_value
        = dx12_group_value{src->_upload_group, src->_pending_async_upload_value.load(std::memory_order_acquire)};
    _actor->enqueue_message(cc::move(job));

    return sg::stream_download_handle(cc::move(control), sg::bytes_future(cc::move(dst), control->completion));
}

sg::stream_download_handle dx12_download_async_system::stream_sink_buffer(sg::raw_buffer_handle buffer,
                                                                          sg::stream_sink sink,
                                                                          isize offset,
                                                                          isize size)
{
    CC_ASSERT(buffer != nullptr, "stream download source buffer is null");
    auto const* const src = dynamic_cast<dx12_buffer const*>(buffer.get());
    CC_ASSERT(src != nullptr, "buffer is not a dx12 buffer");
    CC_ASSERT(!src->is_expired(), "stream download source is a transient buffer used past its epoch (expired)");
    CC_ASSERT(src->_resource, "stream download source buffer has no storage");
    CC_ASSERT(src->usage().has(sg::buffer_usage::copy_src), "stream download source buffer must have "
                                                            "buffer_usage::copy_src");
    CC_ASSERT(_mapped != nullptr, "async download system used before initialization");

    CC_ASSERT(src->_download_group != nullptr, "a download source must have been created with copy_src");
    u64 const value = src->_download_group->reserve();

    auto typed = std::static_pointer_cast<dx12_buffer const>(cc::move(buffer));
    auto control = make_stream_control(typed, value, size);

    dx12_async_download_job job;
    job.buffer_source = typed;
    job.src_offset = offset;
    job.size = size;
    job.sink = std::make_shared<dx12_download_sink>(cc::move(sink));
    job.stream = control;
    job.completion = control->completion;
    job.completion_value = dx12_group_value{src->_download_group, value};
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(std::memory_order_acquire));
    job.upload_wait_value
        = dx12_group_value{src->_upload_group, src->_pending_async_upload_value.load(std::memory_order_acquire)};
    _actor->enqueue_message(cc::move(job));

    // No bytes_future: the sink IS the delivery channel, and handing back an empty future beside it would only
    // invite someone to wait on bytes that were never going to land anywhere.
    return sg::stream_download_handle(cc::move(control), sg::bytes_future());
}

sg::stream_download_handle dx12_download_async_system::stream_sink_texture(sg::raw_texture_handle texture,
                                                                           sg::stream_sink sink,
                                                                           sg::subresource_index const& subresource,
                                                                           sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "stream download source texture is null");
    auto const* const src = dynamic_cast<dx12_texture const*>(texture.get());
    CC_ASSERT(src != nullptr, "texture is not a dx12 texture");
    CC_ASSERT(!src->is_expired(), "stream download source is a transient texture used past its epoch (expired)");
    CC_ASSERT(src->_resource, "stream download source texture has no storage");
    CC_ASSERT(src->usage().has(sg::texture_usage::copy_src), "stream download source texture must have "
                                                             "texture_usage::copy_src");
    CC_ASSERT(_mapped != nullptr, "async download system used before initialization");

    dx12_texture_footprint const fp = compute_texture_footprint(src->description(), subresource, region);

    CC_ASSERT(src->_download_group != nullptr, "a download source must have been created with copy_src");
    u64 const value = src->_download_group->reserve();

    auto typed = std::static_pointer_cast<dx12_texture const>(cc::move(texture));
    auto control = make_stream_control(typed, value, fp.tight_size());

    dx12_async_download_job job;
    job.texture_source = typed;
    job.footprint = fp;
    job.is_texture = true;
    job.sink = std::make_shared<dx12_download_sink>(cc::move(sink));
    job.stream = control;
    job.completion = control->completion;
    job.completion_value = dx12_group_value{src->_download_group, value};
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(std::memory_order_acquire));
    job.upload_wait_value
        = dx12_group_value{src->_upload_group, src->_pending_async_upload_value.load(std::memory_order_acquire)};
    _actor->enqueue_message(cc::move(job));

    return sg::stream_download_handle(cc::move(control), sg::bytes_future());
}

void dx12_download_async_system::set_window_bytes(isize bytes)
{
    CC_ASSERT(bytes > 0, "async download staging window must be positive");
    // Record the request; the copy actor adopts it at the top of its next process cycle (before staging).
    _desired_window_bytes.store(bytes, std::memory_order_release);
}

void dx12_download_async_system::shutdown()
{
    if (_actor)
    {
        _actor->shutdown(); // drains queued reads; on_thread_shutdown waits for the copy queue to idle
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
}
} // namespace sg::backend::dx12
