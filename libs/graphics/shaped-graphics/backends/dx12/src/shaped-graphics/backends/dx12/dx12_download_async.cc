// dx12_download_async_system: the copy actor behind dx12_download_async.hh — window packing, submission, and the in-flight drain.
// The shape and the two fences are on the class doc there.
// Why the drain rule and the acyclicity guard are load-bearing: libs/graphics/shaped-graphics/docs/concepts/download.async.md.

#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_download_async.hh>
#include <shaped-graphics/backends/dx12/dx12_resource_download.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>

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
    std::weak_ptr<void const> pin;                      // future's pin; expired == caller cancelled the copy
    std::shared_ptr<dx12_async_download_waiter> waiter; // set only on a job's last chunk; marks the future ready
    std::shared_ptr<void const> source_keepalive;       // holds the source (buffer or texture) alive across the read
};

// A submitted-but-not-yet-drained window.
// Its GPU read completes when the window fence reaches window_index + 1; its memcpies then run in order and their waiters are marked ready.
struct inflight_window
{
    u64 window_index = 0;
    cc::vector<download_mem_job> mem_jobs;
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
        // Flush anything still buffered, then drain every window — that waits out the copy queue, so the staging buffer and command list/allocators are safe to release.
        for (auto& job : _pending)
            stage_job(job);
        _pending.clear();
        submit_window();
        drain_all();
    }

private:
    // A cancelled read, whose future's pin expired: skip the read entirely — no copy, no forward wait, and no read-after-write hazard to order.
    // But STILL fold the completion value so the download fence reaches it; a later writer stamped with it must not hang.
    // An empty window still submits and signals, keeping the fence monotonic and gap-free.
    void fold_cancelled_completion(dx12_async_download_job& job)
    {
        if (job.completion_value != dx12_download_fence_value::none)
        {
            ensure_open_window();
            u64 const v = u64(job.completion_value);
            if (v > _open_highest_finished)
                _open_highest_finished = v;
        }
    }

    // Resolves one readback's source (buffer or texture) and stages it.
    // The source is held strong for the job's whole lifetime, so its storage survives the read without a deferred-deletion gate.
    void stage_job(dx12_async_download_job& job)
    {
        if (job.pin.expired())
        {
            fold_cancelled_completion(job);
            return;
        }
        if (job.is_texture)
        {
            CC_ASSERT(round_window(job.footprint.padded_pitch) <= _sys._window_bytes, "a single texture row exceeds "
                                                                                      "one staging window");
            dx12_texture_download download(job.texture_source->_resource.Get(), job.footprint, job.dst);
            stage_resource(download, job, job.texture_source);
        }
        else
        {
            dx12_buffer_download download(job.buffer_source->_resource.Get(), job.src_offset, job.dst);
            stage_resource(download, job, job.buffer_source);
        }
    }

    // Packs one resource readback (buffer or texture) into staging windows, submitting each as it fills, and defers each chunk's CPU memcpy to drain.
    // A read larger than a window spans several.
    // A texture job self-aligns each window and returns 0 when a window tail can't fit its next aligned row, which rolls to a fresh window; buffers never return 0.
    // `keepalive` holds the source alive across the deferred copies.
    void stage_resource(dx12_resource_download& download,
                        dx12_async_download_job& job,
                        std::shared_ptr<void const> const& keepalive)
    {
        // A window issues its cross-queue waits once, hoisted ahead of its reads (submit_window).
        // So it must never both promise a completion V and carry a wait that could depend on V — that self-referential pair is the copy-actor deadlock.
        // Two such waits per read: the forward direct-queue token, and the upload completion value.
        // The async upload this read waits on may itself reverse-wait on a direct writer that waits on this window's V.
        // If the open window already finished a read and either wait is still pending, close it now, so this job's waits land in a fresh window pointing only at prior work.
        bool const forward_pending = u64(job.wait_token) > _sys._ctx._submission_fence->GetCompletedValue();
        bool const upload_pending
            = job.upload_wait_value != dx12_copy_fence_value::none
           && u64(job.upload_wait_value) > _sys._ctx._upload_async._completion_fence->GetCompletedValue();
        if (_window_open && _open_highest_finished > 0 && (forward_pending || upload_pending))
            submit_window();

        while (!download.is_finished())
        {
            ensure_open_window();
            isize const avail = _sys._window_bytes - _window_used;
            isize const base = isize(_current_window % u64(num_staging_windows)) * _sys._window_bytes;
            dx12_download_allocation const alloc = {_sys._staging.Get(), _sys._mapped, base + _window_used, avail};

            dx12_pending_copy chunk = download.execute_next_job(*_list.Get(), alloc);
            if (chunk.bytes == 0) // window tail too small for the next aligned texture row → roll to a fresh window
            {
                submit_window();
                continue;
            }
            _window_used += chunk.bytes;

            // This chunk reads the source, so its window must first wait for the last direct-queue list that used it, and for any pending async upload to it.
            // Max over the window; both fences are monotonic.
            if (u64(job.wait_token) > _open_max_wait_token)
                _open_max_wait_token = u64(job.wait_token);
            if (u64(job.upload_wait_value) > _open_max_upload_wait)
                _open_max_upload_wait = u64(job.upload_wait_value);

            // The window holding the read's last byte is the one whose completion satisfies a later writer's reverse wait.
            // Values are monotonic in enqueue order, so max keeps the window's value correct.
            bool const last = download.is_finished();
            if (last && job.completion_value != dx12_download_fence_value::none)
            {
                u64 const v = u64(job.completion_value);
                if (v > _open_highest_finished)
                    _open_highest_finished = v;
            }

            // Defer the CPU memcpy until this window's GPU read completes, at drain.
            // Only the last chunk marks the future ready: windows drain in order, so every earlier chunk is copied by then.
            _open_mem_jobs.push_back(
                download_mem_job{cc::move(chunk.deferred_cpu_copy), job.pin, last ? job.waiter : nullptr, keepalive});

            if (_window_used == _sys._window_bytes) // full → submit now and roll to the next window
                submit_window();
        }
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
        _open_highest_finished = 0;
        _open_max_wait_token = 0;
        _open_max_upload_wait = 0;
        _open_mem_jobs.clear();
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

        // Forward cross-queue sync vs a pending async upload: hold this queue until the upload completion fence reaches the highest upload value the window's reads must observe.
        // A read then never races an in-flight upload to the same buffer.
        // The acyclicity guard above keeps this hoisted wait from ever preceding a read whose completion the upload transitively depends on.
        if (_open_max_upload_wait > 0)
            _sys._copy_queue->Wait(_sys._ctx._upload_async._completion_fence.Get(), _open_max_upload_wait);

        ID3D12CommandList* lists[] = {_list.Get()};
        _sys._copy_queue->ExecuteCommandLists(1, lists);

        // Window fence is 1-based: window i completing signals i+1.
        // The fence starts at 0, so a 0-based value for window 0 would read as "not yet started" and wait_for_window(0) would never block.
        // wait_for_window applies the same +1, so callers pass window indices.
        HRESULT const hs = _sys._copy_queue->Signal(_sys._window_fence.Get(), _current_window + 1);
        CC_ASSERT(SUCCEEDED(hs), "ID3D12CommandQueue::Signal (download window) failed");

        // Completion fence is monotonic: only signal when this window finished a later read than any prior one.
        // Windows carrying only a mid-read chunk finish nothing and skip it.
        if (_open_highest_finished > _last_signaled_download)
        {
            HRESULT const hcf = _sys._copy_queue->Signal(_sys._completion_fence.Get(), _open_highest_finished);
            CC_ASSERT(SUCCEEDED(hcf), "ID3D12CommandQueue::Signal (download completion) failed");
            _last_signaled_download = _open_highest_finished;
        }

        _inflight.push_back(inflight_window{_current_window, cc::move(_open_mem_jobs)});
        _open_mem_jobs = {};
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

    // Drains the oldest in-flight window: waits for its GPU read, runs its memcpies in order (skipping any whose future was dropped), and marks each last-chunk waiter ready.
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

    cc::vector<dx12_async_download_job> _pending; // received this cycle, staged in on_process

    u64 _current_window = 0;         // next window index to submit; slot = index % num_staging_windows
    u64 _last_signaled_download = 0; // highest value signaled on the download completion fence (monotonic)

    // One command list reused across every window; one allocator per window slot, reset when the window three back has drained.
    // Owned here rather than in the epoch-gated pool, since the copy queue does not observe epoch semantics.
    ComPtr<ID3D12GraphicsCommandList> _list;
    ComPtr<ID3D12CommandAllocator> _allocators[num_staging_windows];

    bool _window_open = false;
    isize _window_used = 0;                      // bytes read into the open window so far
    u64 _open_highest_finished = 0;              // highest completion value of reads finished in the open window
    u64 _open_max_wait_token = 0;                // highest direct-queue token the open window's reads must wait for
    u64 _open_max_upload_wait = 0;               // highest async-upload value the open window's reads must wait for
    cc::vector<download_mem_job> _open_mem_jobs; // memcpies accumulated for the open window

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

    if (HRESULT hr = _ctx._device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_window_fence)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateFence (async download window) failed");

    // Download completion fence: signaled by this copy queue when a window's read has finished, and a later direct-queue writer waits on it at submit (see dx12_command_list).
    if (HRESULT hr = _ctx._device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_completion_fence)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateFence (async download completion) failed");

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
        return sg::bytes_future(cc::pinned_data<byte const>(), std::make_shared<sg::ready_bytes_waiter>());

    CC_ASSERT(src->_resource, "async download source buffer has no storage");
    CC_ASSERT(sg::has_flag(src->usage(), sg::buffer_usage::copy_src), "async download source buffer must have "
                                                                      "buffer_usage::copy_src");
    CC_ASSERT(_mapped != nullptr, "async download system used before initialization");

    // Forward cross-queue sync vs a pending async UPLOAD to the same buffer: the read must observe it.
    // Upload and download own independent copy queues, so the read's window waits on the upload completion fence for this value.
    // That wait is issued on the download queue in submit_window — a clean GPU wait, no CPU stall.
    u64 const upload_wait = src->_pending_async_upload_value.load(std::memory_order_acquire);

    // Destination the read bytes land in; the pinned_data keeps it alive until the copy runs (or cancels).
    auto dst = cc::pinned_data<byte>::create_uninitialized(size);
    cc::span<byte> const dst_span = dst.span();
    auto waiter = std::make_shared<dx12_async_download_waiter>();

    // Reserve this read's completion value and stamp the buffer *before* enqueuing, so a later direct-queue list that writes the buffer already sees a value to wait on.
    u64 const value = _next_download_value.fetch_add(1, std::memory_order_relaxed) + 1;
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
    job.waiter = waiter;
    job.completion_value = dx12_download_fence_value(value);
    // Forward sync: defer the read behind the last direct-queue list that used the buffer, so it reads committed bytes and never races an earlier-submitted writer.
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(std::memory_order_acquire));
    job.upload_wait_value = dx12_copy_fence_value(upload_wait); // 0 == none: no pending async upload
    _actor->enqueue_message(cc::move(job));

    return sg::bytes_future(cc::move(dst), cc::move(waiter));
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
    CC_ASSERT(sg::has_flag(src->usage(), sg::texture_usage::copy_src), "async download source texture must have "
                                                                       "texture_usage::copy_src");
    CC_ASSERT(_mapped != nullptr, "async download system used before initialization");

    // The region is already resolved (whole subresource / bounds-checked / empty→skipped) by the sg layer.
    dx12_texture_footprint const fp = compute_texture_footprint(src->description(), subresource, region);

    u64 const upload_wait = src->_pending_async_upload_value.load(std::memory_order_acquire);

    auto dst = cc::pinned_data<byte>::create_uninitialized(fp.tight_size());
    cc::span<byte> const dst_span = dst.span();
    auto waiter = std::make_shared<dx12_async_download_waiter>();

    u64 const value = _next_download_value.fetch_add(1, std::memory_order_relaxed) + 1;
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
    job.waiter = waiter;
    job.completion_value = dx12_download_fence_value(value);
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(std::memory_order_acquire));
    job.upload_wait_value = dx12_copy_fence_value(upload_wait);
    _actor->enqueue_message(cc::move(job));

    return sg::bytes_future(cc::move(dst), cc::move(waiter));
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
    _completion_fence.Reset();
    _copy_queue.Reset();
    if (_wait_event)
    {
        CloseHandle(_wait_event);
        _wait_event = nullptr;
    }
}
} // namespace sg::backend::dx12
