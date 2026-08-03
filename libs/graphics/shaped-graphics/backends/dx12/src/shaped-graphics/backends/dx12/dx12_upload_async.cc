// dx12_upload_async_system: the copy actor behind dx12_upload_async.hh — window packing, submission, and the staging memcpy.
// The shape and the two fences are on the class doc there.
// Source bytes are read only during the memcpy into staging, so a job and its pin die as soon as it is fully staged — on the actor thread, off the submission path.
// Why the acyclicity guard is load-bearing: libs/graphics/shaped-graphics/docs/concepts/upload.async.md.

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_resource_upload.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_copy.hh>
#include <shaped-graphics/backends/dx12/dx12_upload_async.hh>

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

/// The async-upload copy actor: one thread that packs jobs into staging windows and submits copy work.
/// All window / job / command-list state lives here and is touched only on the actor thread, so it needs no locks.
/// It reaches the shared, immutable-after-init fields (staging buffer, fences, queue) via _sys.
class dx12_upload_async_actor final : public cc::threaded_actor_impl<dx12_async_upload_job>
{
public:
    explicit dx12_upload_async_actor(dx12_upload_async_system& sys) : _sys(sys) {}

    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "sg-dx12-upload-async"; }

protected:
    void on_message(dx12_async_upload_job job) override { _pending.push_back(cc::move(job)); }

    bool on_process() override
    {
        maybe_resize_staging(); // adopt a pending set_window_bytes now, while no window is open

        if (_pending.empty())
            return false;

        for (auto& job : _pending)
            stage_job(job);
        _pending.clear(); // destroys jobs → releases pins + buffer handles; bytes are already staged
        submit_window();  // flush the final partial window so its copies run
        return false;     // everything drained + submitted; sleep until the next message
    }

    void on_thread_shutdown() override
    {
        // Flush anything still buffered, then wait for the copy queue to idle.
        // The staging buffer and the command list/allocators are only safe to release afterwards.
        // This queue is independent of the direct queue, which shutdown drains separately.
        for (auto& job : _pending)
            stage_job(job);
        _pending.clear();
        submit_window();
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
        if (job.copy_fence_value != dx12_copy_fence_value::none)
        {
            ensure_open_window();
            u64 const v = u64(job.copy_fence_value);
            if (v > _open_highest_finished)
                _open_highest_finished = v;
        }
    }

    // Resolves one upload's destination (buffer or texture) and stages it.
    // The strong handle is held across the whole staging loop — memcpy and record — and released at return.
    void stage_job(dx12_async_upload_job& job)
    {
        if (job.is_texture)
        {
            auto const strong = job.texture_target.lock();
            if (!strong || !strong->_resource)
            {
                fold_dropped_completion(job);
                return;
            }
            CC_ASSERT(round_window(job.footprint.padded_pitch) <= _sys._window_bytes, "a single texture row exceeds "
                                                                                      "one staging window");
            dx12_texture_upload upload(strong->_resource.Get(), job.footprint, job.src.span());
            stage_resource(upload, job);
        }
        else
        {
            auto const strong = job.buffer_target.lock();
            if (!strong || !strong->_resource)
            {
                fold_dropped_completion(job);
                return;
            }
            dx12_buffer_upload upload(strong->_resource.Get(), job.dst_offset, job.src.span());
            stage_resource(upload, job);
        }
    }

    // Packs one resource upload (buffer or texture) into staging windows, submitting each as it fills.
    // An upload larger than a window spans several.
    // A texture job self-aligns each window and returns 0 when a window tail can't fit its next aligned row, which rolls to a fresh window; buffers never return 0.
    void stage_resource(dx12_resource_upload& upload, dx12_async_upload_job& job)
    {
        // A window issues its reverse-sync wait once, hoisted to the front (submit_window).
        // So it must never both promise a completion V and carry a reverse-wait that could depend on V.
        // That self-referential pair is the copy-actor deadlock: the window's Wait sits ahead of the very copy whose signal the Wait transitively needs.
        // If the open window already finished an upload and this job's reverse token is still pending on the direct queue, close the window now.
        // This job's Wait then lands in a fresh window that can only point at prior, already-submitted windows.
        if (_window_open && _open_highest_finished > 0
            && u64(job.wait_token) > _sys._ctx._submission_fence->GetCompletedValue())
            submit_window();

        while (!upload.is_finished())
        {
            ensure_open_window();
            isize const avail = _sys._window_bytes - _window_used;
            isize const base = isize(_current_window % u64(num_staging_windows)) * _sys._window_bytes;
            dx12_upload_allocation const alloc = {_sys._staging.Get(), _sys._mapped, base + _window_used, avail};

            isize const consumed = upload.execute_next_job(*_list.Get(), alloc);
            if (consumed == 0) // window tail too small for the next aligned texture row → roll to a fresh window
            {
                submit_window();
                continue;
            }
            _window_used += consumed;

            // This chunk writes the destination, so its window must first wait for the last direct-queue list that used it.
            // Max over the window; the submission fence is monotonic.
            if (u64(job.wait_token) > _open_max_wait_token)
                _open_max_wait_token = u64(job.wait_token);

            // The window holding the upload's last byte is the one whose completion satisfies the reader wait.
            if (upload.is_finished() && job.copy_fence_value != dx12_copy_fence_value::none)
            {
                u64 const v = u64(job.copy_fence_value);
                if (v > _open_highest_finished)
                    _open_highest_finished = v;
            }

            if (_window_used == _sys._window_bytes) // full → submit now and roll to the next window
                submit_window();
        }
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
        _open_highest_finished = 0;
        _open_max_wait_token = 0;
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

        // Completion fence is monotonic: only signal when this window finished a later upload than any prior one.
        // Windows carrying only a mid-upload chunk finish nothing and skip it.
        if (_open_highest_finished > _last_signaled_copy)
        {
            HRESULT const hcf = _sys._copy_queue->Signal(_sys._completion_fence.Get(), _open_highest_finished);
            CC_ASSERT(SUCCEEDED(hcf), "ID3D12CommandQueue::Signal (completion) failed");
            _last_signaled_copy = _open_highest_finished;
        }

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

    cc::vector<dx12_async_upload_job> _pending; // received this cycle, staged in on_process

    u64 _current_window = 0;     // next window index to submit; slot = index % num_staging_windows
    u64 _last_signaled_copy = 0; // highest value signaled on the completion fence (monotonic)

    // One command list reused across every window; one allocator per window slot, reset when the window three back has completed.
    // Owned here rather than in the epoch-gated pool, since the copy queue does not observe epoch semantics.
    ComPtr<ID3D12GraphicsCommandList> _list;
    ComPtr<ID3D12CommandAllocator> _allocators[num_staging_windows];

    bool _window_open = false;
    isize _window_used = 0;         // bytes written into the open window so far
    u64 _open_highest_finished = 0; // highest completion value of uploads finished in the open window
    u64 _open_max_wait_token = 0;   // highest direct-queue token the open window's copies must wait for
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

    if (HRESULT hr = _ctx._device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_window_fence)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateFence (async upload window) failed");

    // Upload completion fence: signaled by this copy queue when a window's copy has run, and a later direct-queue reader waits on it at submit (see dx12_command_list).
    if (HRESULT hr = _ctx._device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_completion_fence)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateFence (async upload completion) failed");

    _wait_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (_wait_event == nullptr)
        return cc::error("CreateEventW failed for the async upload wait event");

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
    CC_ASSERT(sg::has_flag(dst->usage(), sg::buffer_usage::copy_dst), "async upload target buffer must have "
                                                                      "buffer_usage::copy_dst");
    CC_ASSERT(_mapped != nullptr, "async upload system used before initialization");

    // Reserve this upload's completion value and stamp the destination *before* enqueuing, so any command list reading the buffer after this call already sees a value to wait on.
    u64 const value = _next_copy_value.fetch_add(1, std::memory_order_relaxed) + 1;
    u64 prev = dst->_pending_async_upload_value.load(std::memory_order_relaxed);
    while (prev < value
           && !dst->_pending_async_upload_value.compare_exchange_weak(prev, value, std::memory_order_release,
                                                                      std::memory_order_relaxed))
    {
        // CAS retries; `prev` is refreshed with the current value each time.
    }

    dx12_async_upload_job job;
    // Weak ref, not strong: a caller may drop its last handle before the actor stages this.
    // Resolving at stage time lets a released buffer skip the copy and its staging cost — see stage_job.
    // The storage stays alive independently until the copy fence reaches `value`, which is the lifetime gate.
    job.buffer_target = std::static_pointer_cast<dx12_buffer const>(cc::move(buffer)); // dst already dynamic_cast-verified
    job.dst_offset = offset;
    job.src = cc::move(data);
    job.copy_fence_value = dx12_copy_fence_value(value);
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
    CC_ASSERT(sg::has_flag(dst->usage(), sg::texture_usage::copy_dst), "async upload target texture must have "
                                                                       "texture_usage::copy_dst");
    CC_ASSERT(_mapped != nullptr, "async upload system used before initialization");

    // The region is already resolved (whole subresource / bounds-checked / empty→skipped) by the sg layer.
    dx12_texture_footprint const fp = compute_texture_footprint(dst->description(), subresource, region);
    CC_ASSERT(data.size() == fp.tight_size(), "async upload pixel data size does not match the copy region");

    // Reserve this upload's completion value and stamp the destination before enqueuing, so a later command list reading the texture already sees a value to wait on.
    u64 const value = _next_copy_value.fetch_add(1, std::memory_order_relaxed) + 1;
    u64 prev = dst->_pending_async_upload_value.load(std::memory_order_relaxed);
    while (prev < value
           && !dst->_pending_async_upload_value.compare_exchange_weak(prev, value, std::memory_order_release,
                                                                      std::memory_order_relaxed))
    {
        // CAS retries; `prev` refreshed each time.
    }

    dx12_async_upload_job job;
    job.texture_target = std::static_pointer_cast<dx12_texture const>(cc::move(texture));
    job.footprint = fp;
    job.is_texture = true;
    job.src = cc::move(data);
    job.copy_fence_value = dx12_copy_fence_value(value);
    // Reverse sync: defer the copy behind the last direct-queue list that used this texture.
    job.wait_token = sg::submission_token(dst->_last_used_submission_token.load(std::memory_order_acquire));
    _actor->enqueue_message(cc::move(job));
}

void dx12_upload_async_system::set_window_bytes(isize bytes)
{
    CC_ASSERT(bytes > 0, "async upload staging window must be positive");
    // Record the request; the copy actor adopts it at the top of its next process cycle (before staging).
    _desired_window_bytes.store(bytes, std::memory_order_release);
}

void dx12_upload_async_system::shutdown()
{
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
    _completion_fence.Reset();
    _copy_queue.Reset();
    if (_wait_event)
    {
        CloseHandle(_wait_event);
        _wait_event = nullptr;
    }
}
} // namespace sg::backend::dx12
