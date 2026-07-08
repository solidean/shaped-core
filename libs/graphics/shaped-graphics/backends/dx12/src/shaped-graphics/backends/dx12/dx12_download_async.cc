// dx12_download_async_system: a dedicated COPY queue that streams GPU→CPU buffer readback off the frame
// path. A cc::threaded_actor drains download jobs, records CopyBufferRegion from the source into a
// persistently-mapped READBACK staging buffer, then memcpys the staged bytes into the caller's destination
// and marks the future ready.
//
// The staging buffer is triple-buffered into fixed windows so GPU read and CPU memcpy overlap: while the
// GPU reads window N the actor fills window N+1, with window N+2 as slack. A window is submitted as soon as
// it fills (or the inbox drains); a read larger than a window packs across successive windows. Because a
// download completes only once the CPU memcpy has run, each submitted window is kept in flight until it is
// drained — the actor waits on that window's staging fence, memcpys its chunks, and marks their waiters
// ready — which happens before its slot is reused and for every remaining window when the inbox empties (so
// a future always becomes ready without an epoch advance). A second fence, the context's download
// completion fence, is signaled with the highest finished read value each window; the submit path makes a
// later direct-queue list that WRITES the buffer wait on it, so it never overwrites bytes the read is still
// reading (the reverse cross-queue sync). The system owns one copy command list (reused across windows) and
// one allocator per window slot, cycled on the window fence — not the epoch-gated pool, since the copy
// queue does not observe epoch semantics. The source buffer is held strong for a job's whole lifetime, so
// its storage survives the read without a deferred-deletion gate. See
// libs/graphics/shaped-graphics/docs/concepts/download.async.md.

#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/print.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_download_async.hh>
#include <shaped-graphics/backends/dx12/dx12_resource_download.hh>

namespace sg::backend::dx12
{
namespace
{
// Triple-buffered staging: one window read by the GPU, one just submitted, one being filled by the CPU.
// Fewer than three reintroduces a sync bubble; more only adds staging memory.
constexpr int num_staging_windows = 3;

// One chunk's CPU memcpy out of the readback staging buffer, deferred until its window's GPU read has run.
// `source_keepalive` holds the read source alive until then (the read is recorded but not yet executed
// when the job is dropped from the inbox).
struct download_mem_job
{
    cc::unique_function<void()> deferred_cpu_copy;
    std::weak_ptr<void const> pin;                      // future's pin; expired == caller cancelled the copy
    std::shared_ptr<dx12_async_download_waiter> waiter; // set only on a job's last chunk; marks the future ready
    std::shared_ptr<dx12_buffer const> source_keepalive;
};

// A submitted-but-not-yet-drained window. Its GPU read completes when the window fence reaches
// window_index + 1; then its memcpies run in order and their waiters are marked ready.
struct inflight_window
{
    cc::u64 window_index = 0;
    cc::vector<download_mem_job> mem_jobs;
};

/// The async-download copy actor: one thread that packs readbacks into staging windows, submits them, and
/// drains completed windows into their destinations. All window / job / command-list state lives here and
/// is touched only on the actor thread, so it needs no locks. It reaches shared, immutable-after-init
/// fields (staging buffer, fences, queue) via _sys.
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

        if (_pending.empty())
            return false;

        for (auto& job : _pending)
            stage_job(job);
        _pending.clear(); // staged reads moved their source into mem_jobs; cancelled ones drop it here
        submit_window();  // flush the final partial window so its reads run
        drain_all();      // wait + memcpy every in-flight window so all futures are ready before we sleep
        return false;     // everything drained + delivered; sleep until the next message
    }

    void on_thread_shutdown() override
    {
        // Flush anything still buffered, then drain every window (waits for the copy queue to idle) so the
        // staging buffer + command list/allocators are safe to release.
        for (auto& job : _pending)
            stage_job(job);
        _pending.clear();
        submit_window();
        drain_all();
    }

private:
    // Packs one readback into staging windows, submitting each as it fills. A read larger than a window
    // spans several; a job finishing mid-window carries its completion value + last-chunk waiter into it.
    void stage_job(dx12_async_download_job& job)
    {
        // Cancelled? A dropped future expires its pin. Skip the read (no CopyBufferRegion, no forward wait —
        // no read means no read-after-write hazard) but STILL fold the completion value so the download
        // fence reaches it: a later writer stamped with this value must not hang. Mirrors upload's dropped-
        // target rule. An empty window still submits + signals, keeping the fence monotonic and gap-free.
        if (job.pin.expired())
        {
            if (job.completion_value != dx12_download_fence_value::none)
            {
                ensure_open_window();
                cc::u64 const v = cc::u64(job.completion_value);
                if (v > _open_highest_finished)
                    _open_highest_finished = v;
            }
            return;
        }

        // A window issues its forward-sync wait once, hoisted ahead of its reads (submit_window), so it must
        // never both promise a completion V and carry a forward-wait that could depend on V — that self-
        // referential pair is the copy-actor deadlock. If the open window already finished a read and this
        // job's forward token is still pending on the direct queue, close the window now: this job's Wait
        // then lands in a fresh window that can only point at prior, already-submitted work.
        if (_window_open && _open_highest_finished > 0
            && cc::u64(job.wait_token) > _sys._ctx._submission_fence->GetCompletedValue())
        {
            submit_window();
        }

        dx12_buffer_download download(job.source->_resource.Get(), job.src_offset, job.dst);
        while (!download.is_finished())
        {
            ensure_open_window();
            cc::isize const avail = _sys._window_bytes - _window_used;
            CC_ASSERT(avail > 0, "open staging window has no room"); // ensure_open_window guarantees room
            cc::isize const base = cc::isize(_current_window % cc::u64(num_staging_windows)) * _sys._window_bytes;
            dx12_download_allocation const alloc{_sys._staging.Get(), _sys._mapped, base + _window_used, avail};

            dx12_pending_copy chunk = download.execute_next_job(*_list.Get(), alloc);
            CC_ASSERT(chunk.bytes > 0, "async download made no progress");
            _window_used += chunk.bytes;

            // This chunk reads the buffer, so its window must first wait for the last direct-queue list that
            // used it (forward sync). Max over the window; the submission fence is monotonic.
            if (cc::u64(job.wait_token) > _open_max_wait_token)
                _open_max_wait_token = cc::u64(job.wait_token);

            // The window holding a read's last byte is the one whose completion satisfies a later writer's
            // reverse wait. Values are monotonic in enqueue order, so max keeps the window's value correct.
            bool const last = download.is_finished();
            if (last && job.completion_value != dx12_download_fence_value::none)
            {
                cc::u64 const v = cc::u64(job.completion_value);
                if (v > _open_highest_finished)
                    _open_highest_finished = v;
            }

            // Defer the CPU memcpy until this window's GPU read completes (at drain). The source is kept
            // alive until then via the handle copy. Only the last chunk marks the future ready — windows
            // drain in order, so by then every earlier chunk has already been copied.
            _open_mem_jobs.push_back(
                download_mem_job{cc::move(chunk.deferred_cpu_copy), job.pin, last ? job.waiter : nullptr, job.source});

            if (_window_used == _sys._window_bytes) // full → submit now and roll to the next window
                submit_window();
        }
    }

    // Ensures a window is open with room to write. One command list is reused across all windows; each of
    // the three window slots has its own allocator. Reusing a slot first fully drains the window three
    // submissions back — its previous occupant — so its staging memory, its allocator's GPU work, AND its
    // CPU memcpies are done before the slot is overwritten.
    void ensure_open_window()
    {
        if (_window_open)
            return;

        int const slot = int(_current_window % cc::u64(num_staging_windows));
        if (_current_window >= cc::u64(num_staging_windows))
            drain_until(_current_window - cc::u64(num_staging_windows));

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
        _open_highest_finished = 0;
        _open_max_wait_token = 0;
        _open_mem_jobs.clear();
        _window_open = true;
    }

    // Closes + submits the open window: hoists its forward-sync wait, executes it, signals the window fence
    // (its slot is reusable once drained) and, if it finished any read, the download completion fence up to
    // the highest finished value. The window's memcpies are deferred — pushed to the in-flight FIFO to be
    // drained later. No-op if no window is open.
    void submit_window()
    {
        if (!_window_open)
            return;

        HRESULT const hc = _list->Close();
        CC_ASSERT(SUCCEEDED(hc), "ID3D12GraphicsCommandList::Close failed");

        // Forward sync: hold the copy queue until every direct-queue list that used this window's source
        // buffers has finished, so the read sees committed bytes and never races an in-flight writer.
        // Over-waiting on a higher (monotonic) token is safe; an already-completed token returns at once.
        if (_open_max_wait_token > 0)
            _sys._ctx._copy_queue->Wait(_sys._ctx._submission_fence.Get(), _open_max_wait_token);

        ID3D12CommandList* lists[] = {_list.Get()};
        _sys._ctx._copy_queue->ExecuteCommandLists(1, lists);

        // Window fence is 1-based (window i completing signals i+1): the fence starts at 0, so a 0-based
        // value for window 0 would be indistinguishable from "not yet started" and wait_for_window(0) would
        // never block. wait_for_window applies the same +1, so callers pass window indices.
        HRESULT const hs = _sys._ctx._copy_queue->Signal(_sys._window_fence.Get(), _current_window + 1);
        CC_ASSERT(SUCCEEDED(hs), "ID3D12CommandQueue::Signal (download window) failed");

        // Completion fence is monotonic: only signal when this window finished a later read than any prior
        // window. Windows carrying only a mid-read chunk finish nothing and skip it.
        if (_open_highest_finished > _last_signaled_download)
        {
            HRESULT const hcf = _sys._ctx._copy_queue->Signal(_sys._completion_fence.Get(), _open_highest_finished);
            CC_ASSERT(SUCCEEDED(hcf), "ID3D12CommandQueue::Signal (download completion) failed");
            _last_signaled_download = _open_highest_finished;
        }

        _inflight.push_back(inflight_window{_current_window, cc::move(_open_mem_jobs)});
        _open_mem_jobs = {};
        _window_open = false;
        ++_current_window;
    }

    // Drains every in-flight window whose index is <= `last_index_inclusive` (FIFO, oldest first).
    void drain_until(cc::u64 last_index_inclusive)
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

    // Drains the oldest in-flight window: waits for its GPU read to complete, runs its memcpies in order
    // (skipping any whose future was dropped), and marks each last-chunk waiter ready.
    void drain_front()
    {
        inflight_window w = cc::move(_inflight[0]);
        _inflight.remove_from_to(0, 1);

        wait_for_window(w.window_index);
        for (auto& mj : w.mem_jobs)
        {
            if (auto const alive = mj.pin.lock()) // future still wants the data?
                mj.deferred_cpu_copy();
            if (mj.waiter)
                mj.waiter->mark_ready();
        }
    }

    // Adopts a pending set_window_bytes if one differs from the current size. Called at the top of a
    // process cycle, when no window is open. Submits any open window, drains every in-flight window (each
    // memcpys out of the old staging buffer, so the buffer cannot be freed until they are all done), then
    // rebuilds staging at the new size. The per-slot allocators and the reused command list survive.
    void maybe_resize_staging()
    {
        cc::isize const desired = _sys._desired_window_bytes.load(std::memory_order_acquire);
        if (desired == _sys._window_bytes)
            return;
        CC_ASSERT(desired > 0, "async download staging window must be positive");

        submit_window();
        drain_all();

        // Build the new staging buffer before releasing the old, so a failed allocation leaves the current
        // one intact (the resize is then simply not applied; the next cycle retries).
        auto ring = create_mapped_ring_buffer(_sys._ctx._device.Get(), D3D12_HEAP_TYPE_READBACK,
                                              D3D12_RESOURCE_STATE_COPY_DEST, desired * num_staging_windows);
        CC_ASSERT(ring.has_value(), "async download staging resize failed to allocate");

        _sys._staging->Unmap(0, nullptr);
        _sys._staging = cc::move(ring.value().resource);
        _sys._mapped = static_cast<cc::byte*>(ring.value().mapped);
        _sys._window_bytes = desired;
    }

    // Blocks the actor until the copy queue has finished `window` (index). The fence is 1-based (see
    // submit_window), so window i's completion is fence value i+1 — distinct from the initial 0.
    void wait_for_window(cc::u64 window)
    {
        cc::u64 const target = window + 1;
        if (_sys._window_fence->GetCompletedValue() < target)
        {
            HRESULT const hr = _sys._window_fence->SetEventOnCompletion(target, _sys._wait_event);
            CC_ASSERT(SUCCEEDED(hr), "ID3D12Fence::SetEventOnCompletion failed");
            WaitForSingleObject(_sys._wait_event, INFINITE);
        }
    }

    dx12_download_async_system& _sys;

    cc::vector<dx12_async_download_job> _pending; // received this cycle, staged in on_process

    cc::u64 _current_window = 0;         // next window index to submit; slot = index % num_staging_windows
    cc::u64 _last_signaled_download = 0; // highest value signaled on the download completion fence (monotonic)

    // One command list reused across every window; one allocator per window slot, cycled and reset when the
    // window three back has drained. Owned here (not the epoch-gated pool): the copy queue does not observe
    // epoch semantics.
    ComPtr<ID3D12GraphicsCommandList> _list;
    ComPtr<ID3D12CommandAllocator> _allocators[num_staging_windows];

    bool _window_open = false;
    cc::isize _window_used = 0;                  // bytes read into the open window so far
    cc::u64 _open_highest_finished = 0;          // highest completion value of reads finished in the open window
    cc::u64 _open_max_wait_token = 0;            // highest direct-queue token the open window's reads must wait for
    cc::vector<download_mem_job> _open_mem_jobs; // memcpies accumulated for the open window

    cc::vector<inflight_window> _inflight; // submitted, not-yet-drained windows (FIFO, oldest at the front)
};
} // namespace

cc::result<cc::unit> dx12_download_async_system::initialize(cc::isize window_bytes)
{
    CC_ASSERT(window_bytes > 0, "async download staging window must be positive");
    CC_ASSERT(_ctx._copy_queue, "async download needs the context's shared copy queue first");

    // READBACK heap, COPY_DEST: the copy queue writes read bytes here via CopyBufferRegion, the actor reads
    // them back out on the CPU. Three windows back-to-back, addressed by (window index % 3) * window_bytes.
    auto staging = create_mapped_ring_buffer(_ctx._device.Get(), D3D12_HEAP_TYPE_READBACK,
                                             D3D12_RESOURCE_STATE_COPY_DEST, window_bytes * num_staging_windows);
    CC_RETURN_IF_ERROR(staging);
    _staging = cc::move(staging.value().resource);
    _mapped = static_cast<cc::byte*>(staging.value().mapped);
    _window_bytes = window_bytes;
    _desired_window_bytes.store(window_bytes, std::memory_order_relaxed); // no resize pending yet

    if (HRESULT hr = _ctx._device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_window_fence)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateFence (async download window) failed");

    // Download completion fence: signaled by the shared copy queue when a window's read has finished; a
    // later direct-queue writer waits on it at submit (see dx12_command_list).
    if (HRESULT hr = _ctx._device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_completion_fence)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateFence (async download completion) failed");

    _wait_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (_wait_event == nullptr)
        return cc::error("CreateEventW failed for the async download wait event");

    _actor = cc::make_and_start_threaded_actor<dx12_download_async_actor>(*this);
    return cc::unit{};
}

sg::bytes_future dx12_download_async_system::download_buffer(sg::raw_buffer_handle buffer, cc::isize offset, cc::isize size)
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
        return sg::bytes_future(cc::pinned_data<cc::byte const>(), std::make_shared<sg::ready_bytes_waiter>());

    CC_ASSERT(src->_resource, "async download source buffer has no storage");
    CC_ASSERT(sg::has_flag(src->usage(), sg::buffer_usage::copy_src), "async download source buffer must have "
                                                                      "buffer_usage::copy_src");
    CC_ASSERT(_mapped != nullptr, "async download system used before initialization");

    // Fringe: a pending async UPLOAD to this buffer must be visible to the read. Upload + download share the
    // copy queue but run on independent actors, so they can't be cheaply ordered on the GPU — block on the
    // upload's completion value (a documented v1 simplification; see
    // libs/graphics/shaped-graphics/docs/concepts/download.async.md).
    wait_for_pending_async_upload(*src);

    // Destination the read bytes land in; the pinned_data keeps it alive until the copy runs (or cancels).
    auto dst = cc::pinned_data<cc::byte>::create_uninitialized(size);
    cc::span<cc::byte> const dst_span = dst.span();
    auto waiter = std::make_shared<dx12_async_download_waiter>();

    // Reserve this read's completion value and stamp the buffer *before* enqueuing, so a later direct-queue
    // list that writes the buffer already sees a value to wait on (the reverse cross-queue sync).
    cc::u64 const value = _next_download_value.fetch_add(1, std::memory_order_relaxed) + 1;
    cc::u64 prev = src->_pending_async_download_value.load(std::memory_order_relaxed);
    while (prev < value
           && !src->_pending_async_download_value.compare_exchange_weak(prev, value, std::memory_order_release,
                                                                        std::memory_order_relaxed))
    {
        // CAS retries; `prev` is refreshed with the current value each time.
    }

    dx12_async_download_job job;
    // Held strong for the job's whole lifetime, so the source storage survives the copy-queue read. dst
    // already dynamic_cast-verified above.
    job.source = std::static_pointer_cast<dx12_buffer const>(cc::move(buffer));
    job.src_offset = offset;
    job.size = size;
    job.dst = dst_span;
    job.pin = std::weak_ptr<void const>(dst.pin()); // weak: dropping the future cancels the copy
    job.waiter = waiter;
    job.completion_value = dx12_download_fence_value(value);
    // Forward sync: defer the read behind the last direct-queue list that used the buffer, so it reads
    // committed bytes and never races an earlier-submitted writer.
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(std::memory_order_acquire));
    _actor->enqueue_message(cc::move(job));

    return sg::bytes_future(cc::move(dst), cc::move(waiter));
}

void dx12_download_async_system::wait_for_pending_async_upload(dx12_buffer const& src)
{
    cc::u64 const v = src._pending_async_upload_value.load(std::memory_order_acquire);
    // The upload system owns the upload completion fence; both subsystems share the context's copy queue.
    ID3D12Fence* const upload_fence = _ctx._upload_async._completion_fence.Get();
    if (v == 0 || upload_fence->GetCompletedValue() >= v)
        return;

    cc::eprintln("[sg] async download from a buffer with a pending async upload — blocking the calling thread on the "
                 "upload. Prefer an intervening command-list op, or wait for uploads before downloading.");

    HANDLE const ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CC_ASSERT(ev != nullptr, "CreateEventW failed for the async download upload-wait");
    HRESULT const hr = upload_fence->SetEventOnCompletion(v, ev);
    CC_ASSERT(SUCCEEDED(hr), "ID3D12Fence::SetEventOnCompletion failed");
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
}

void dx12_download_async_system::set_window_bytes(cc::isize bytes)
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
    _completion_fence.Reset();
    if (_wait_event)
    {
        CloseHandle(_wait_event);
        _wait_event = nullptr;
    }
}
} // namespace sg::backend::dx12
