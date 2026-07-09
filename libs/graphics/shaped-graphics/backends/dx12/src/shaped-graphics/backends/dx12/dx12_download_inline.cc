// dx12_download_inline_system: the inline READBACK ring buffer plus the actor that performs deferred
// CPU copies. Downloads record a GPU readback copy at record time; at submit the deferred copies are
// stamped with the list's submission token and enqueued on a cc::threaded_actor. The actor blocks on
// the submission fence, memcpys the readback bytes into the destination (skipping it if the future was
// dropped), marks the waiter ready, and releases the epoch's outstanding-copy count.
//
// Ring space is reclaimed at EPOCH granularity, not per submission. Reservations happen concurrently
// in allocation order; multiple lists record in parallel, so submission order (the actor's copy order)
// need not match allocation order. Freeing a window per submission could release space an interleaved,
// not-yet-submitted list still holds. So each epoch carries an outstanding-copy counter, and a closed
// epoch's whole ring span frees only once that counter hits zero — i.e. every one of its downloads has
// drained (or its list was dropped). See libs/graphics/shaped-graphics/docs/concepts/download.inline.md.

#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_download_inline.hh>
#include <shaped-graphics/backends/dx12/dx12_resource_download.hh>

#include <memory>

namespace sg::backend::dx12
{
namespace
{
/// The download system's async agent: one thread that drains readback copies in submission order.
class dx12_download_actor final : public cc::threaded_actor_impl<dx12_download_copy_job>
{
public:
    explicit dx12_download_actor(dx12_download_inline_system& sys) : _sys(sys) {}

    [[nodiscard]] cc::string_view actor_name() const noexcept override { return "sg-dx12-download"; }

protected:
    void on_message(dx12_download_copy_job job) override
    {
        _sys.wait_for_submission(job.token);   // block until the recording list has run on the GPU
        if (auto const alive = job.pin.lock()) // future still wants the data?
            job.deferred_cpu_copy();
        if (job.waiter)
            job.waiter->mark_ready();
        _sys.on_copy_done(job.epoch_copies); // release this download's hold on its epoch's ring span
    }

private:
    dx12_download_inline_system& _sys;
};
} // namespace

cc::result<cc::unit> dx12_download_inline_system::initialize(cc::isize capacity)
{
    CC_ASSERT(capacity > 0, "download ring capacity must be positive");

    // READBACK heap, COPY_DEST: the GPU writes readback bytes here via CopyBufferRegion, the actor
    // reads them back out on the CPU.
    auto ring = create_mapped_ring_buffer(_ctx._device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                          capacity);
    CC_RETURN_IF_ERROR(ring);
    _buffer = cc::move(ring.value().resource);
    _mapped = static_cast<cc::byte*>(ring.value().mapped);
    _capacity = capacity;

    _wait_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (_wait_event == nullptr)
        return cc::error("CreateEventW failed for the download wait event");

    _ring.lock([](ring_state& s) { s.current_epoch_copies = std::make_shared<std::atomic<cc::isize>>(0); });

    _actor = cc::make_and_start_threaded_actor<dx12_download_actor>(*this);
    return cc::unit{};
}

void dx12_download_inline_system::ring_state::reclaim(dx12_download_inline_system& sys)
{
    cc::u64 const prev = sys._freed_pos.load(std::memory_order_acquire);
    cc::u64 new_freed = prev;

    // Free every leading epoch whose copies have all drained. The FIFO is ordered by allocation, so a
    // still-busy epoch blocks reclaim of everything reserved after it.
    cc::isize retired = 0;
    for (auto const& cp : checkpoints)
    {
        if (cp.outstanding->load(std::memory_order_acquire) != 0)
            break;
        new_freed = cp.end_pos;
        ++retired;
    }
    if (retired > 0)
        checkpoints.remove_from_to(0, retired);

    if (new_freed > prev)
    {
        sys._freed_pos.store(new_freed, std::memory_order_release);
        sys._freed_pos.notify_all();
    }
}

dx12_download_inline_system::reservation dx12_download_inline_system::reserve(cc::isize size)
{
    CC_ASSERT(size > 0, "reserve size must be positive");
    CC_ASSERT(size <= _capacity, "a single inline download exceeds the readback ring capacity");

    for (;;)
    {
        cc::optional<reservation> r = _ring.lock(
            [&](ring_state& s) -> cc::optional<reservation>
            {
                s.reclaim(*this); // pick up epochs the actor has drained since we last looked

                cc::u64 const start = s.next_pos;
                cc::isize const offset = cc::isize(start % cc::u64(_capacity));
                // Never cross the seam: grant only up to the ring end. A request that would wrap is
                // split here — the caller loops and reserves the remainder from offset 0. No tail waste.
                cc::isize const granted = cc::min(size, _capacity - offset);
                cc::u64 const end = start + cc::u64(granted);
                if (end - _freed_pos.load(std::memory_order_acquire) > cc::u64(_capacity))
                {
                    // The window overlaps space still held by earlier epochs. Only closed epochs
                    // (checkpoints) can release it; if there are none, this one open epoch's downloads
                    // exceed the ring — a hard budget error rather than a deadlock (a documented v1
                    // limitation, like the upload ring).
                    CC_ASSERT(!s.checkpoints.empty(), "inline downloads in one epoch exceed the readback ring "
                                                      "capacity");
                    return {};
                }
                s.next_pos = end;
                return reservation{offset, granted, s.current_epoch_copies};
            });

        if (r.has_value())
            return cc::move(r.value());

        // Wait for the actor to drain an earlier epoch and advance the free watermark. Safe to block
        // the recording thread: the occupied space belongs to already-submitted lists the actor is
        // draining. A single epoch whose own downloads exceed the ring asserts above instead.
        cc::u64 const seen = _freed_pos.load(std::memory_order_acquire);
        _freed_pos.wait(seen, std::memory_order_acquire);
    }
}

void dx12_download_inline_system::account_pending_copy(std::shared_ptr<std::atomic<cc::isize>> const& epoch_copies)
{
    // One more outstanding copy, counted only for a reservation that actually produces a copy job (a
    // seam-skip window makes no progress and is not counted). Bumped here rather than in reserve() so the
    // counts pair 1:1 with pushed jobs — each is released in on_copy_done / discard_unsubmitted.
    epoch_copies->fetch_add(1, std::memory_order_relaxed); // this epoch's tally (gates ring reclaim)
    _outstanding.fetch_add(1, std::memory_order_relaxed);  // and the global drain gate
}

dx12_download_inline_system::span_reservation dx12_download_inline_system::reserve_span(cc::isize total)
{
    CC_ASSERT(total > 0, "reserve size must be positive");
    CC_ASSERT(total <= _capacity, "a single inline readback (with staging slack) exceeds the readback ring capacity");

    for (;;)
    {
        cc::optional<span_reservation> r = _ring.lock(
            [&](ring_state& s) -> cc::optional<span_reservation>
            {
                s.reclaim(*this); // pick up epochs the actor has drained since we last looked

                cc::u64 const start = s.next_pos;
                cc::u64 const end = start + cc::u64(total);
                if (end - _freed_pos.load(std::memory_order_acquire) > cc::u64(_capacity))
                {
                    CC_ASSERT(!s.checkpoints.empty(), "inline downloads in one epoch exceed the readback ring "
                                                      "capacity");
                    return {};
                }
                s.next_pos = end;
                return span_reservation{start, s.current_epoch_copies};
            });

        if (r.has_value())
            return cc::move(r.value());

        cc::u64 const seen = _freed_pos.load(std::memory_order_acquire);
        _freed_pos.wait(seen, std::memory_order_acquire);
    }
}

sg::bytes_future dx12_download_inline_system::download_texture(dx12_command_list& cmd,
                                                               ID3D12Resource* src,
                                                               dx12_texture_footprint const& fp)
{
    cc::isize const tight = fp.tight_size();
    if (tight == 0)
        return sg::bytes_future(cc::pinned_data<cc::byte const>(), std::make_shared<sg::ready_bytes_waiter>());

    CC_ASSERT(_mapped != nullptr, "download system used before initialization");

    auto dst = cc::pinned_data<cc::byte>::create_uninitialized(tight);
    cc::span<cc::byte> const dst_span = dst.span();
    auto waiter = std::make_shared<dx12_download_waiter>();

    dx12_texture_download download(src, fp, dst_span);

    // The job self-aligns each byte window and returns bytes-consumed-including-alignment; the ring stays a
    // plain byte allocator. Reserve the whole region once, plus slack for the self-alignment (512) and the one
    // partial row a seam wrap pushes past the boundary (padded), then walk it with to-seam windows (mirroring
    // the inline upload / the legacy gfx backend). Each chunk is its own deferred un-pad copy; a window too
    // small for an aligned row yields an empty copy we skip; only the last real chunk carries the waiter.
    cc::isize const total = download.remaining_bytes() + fp.padded_pitch + texture_placement_alignment;
    CC_ASSERT(total <= _capacity, "an inline texture readback (with staging slack) exceeds the readback ring capacity");

    span_reservation const span = reserve_span(total);
    cc::u64 cursor = span.start;
    while (!download.is_finished())
    {
        cc::isize const offset = cc::isize(cursor % cc::u64(_capacity));
        cc::isize const budget = _capacity - offset; // contiguous bytes to the seam
        dx12_download_allocation const alloc{_buffer.Get(), _mapped, offset, budget};
        dx12_pending_copy pending = download.execute_next_job(*cmd._list.Get(), alloc);
        if (pending.bytes == 0) // tail too small for an aligned row → skip to the seam
        {
            cursor += cc::u64(budget);
            continue;
        }
        cursor += cc::u64(pending.bytes);

        account_pending_copy(span.epoch_copies);

        dx12_download_copy_job job;
        job.deferred_cpu_copy = cc::move(pending.deferred_cpu_copy);
        job.pin = std::weak_ptr<void const>(dst.pin());
        job.waiter = download.is_finished() ? waiter : nullptr;
        job.epoch_copies = span.epoch_copies;
        cmd._pending_downloads.push_back(cc::move(job));
    }

    return sg::bytes_future(cc::move(dst), cc::move(waiter));
}

sg::bytes_future dx12_download_inline_system::download_buffer(dx12_command_list& cmd,
                                                              dx12_buffer const& src,
                                                              cc::isize offset,
                                                              cc::isize size)
{
    if (size == 0)
        return sg::bytes_future(cc::pinned_data<cc::byte const>(), std::make_shared<sg::ready_bytes_waiter>());

    CC_ASSERT(_mapped != nullptr, "download system used before initialization");

    // Destination the readback bytes land in; the pinned_data keeps it alive until the copy runs (or cancels).
    auto dst = cc::pinned_data<cc::byte>::create_uninitialized(size);
    cc::span<cc::byte> const dst_span = dst.span();
    auto waiter = std::make_shared<dx12_download_waiter>();

    dx12_buffer_download download(src, offset, dst_span);
    download.prepare(cmd);

    // One pass per contiguous ring slice: a read that fits without wrapping is a single chunk; one that
    // straddles the seam is split across successive reservations. Each chunk gets its own deferred
    // memcpy and its own epoch-copy count; only the last chunk carries the waiter, so the future
    // becomes ready once every chunk has drained (the actor copies in enqueue order).
    while (!download.is_finished())
    {
        cc::isize const remaining = download.total_bytes() - download.consumed();
        reservation res = reserve(remaining);
        dx12_download_allocation const alloc{_buffer.Get(), _mapped, res.offset, res.granted};
        dx12_pending_copy pending = download.execute_next_job(*cmd._list.Get(), alloc);
        account_pending_copy(res.epoch_copies);

        dx12_download_copy_job job;
        job.deferred_cpu_copy = cc::move(pending.deferred_cpu_copy);
        job.pin = std::weak_ptr<void const>(dst.pin());
        job.waiter = download.is_finished() ? waiter : nullptr;
        job.epoch_copies = cc::move(res.epoch_copies);
        cmd._pending_downloads.push_back(cc::move(job));
    }

    return sg::bytes_future(cc::move(dst), cc::move(waiter));
}

void dx12_download_inline_system::enqueue_submitted(sg::submission_token token, cc::vector<dx12_download_copy_job>& jobs)
{
    for (auto& job : jobs)
    {
        job.token = token;
        if (job.waiter)
            job.waiter->submitted.store(true, std::memory_order_release);
        _actor->enqueue_message(cc::move(job));
    }
    jobs.clear();
}

void dx12_download_inline_system::discard_unsubmitted(cc::vector<dx12_download_copy_job>& jobs)
{
    // Never submitted: the deferred copies won't run and their futures can't complete. Cancel each
    // future and release the epoch-copy count it held, so the epoch still reaches zero and reclaims its
    // ring span once its *submitted* downloads drain. The reserved bytes are not freed individually —
    // they sit inside the open epoch's span and are reclaimed with it.
    bool released = false;
    for (auto& job : jobs)
    {
        if (job.waiter)
            job.waiter->mark_cancelled();
        if (job.epoch_copies)
        {
            job.epoch_copies->fetch_sub(1, std::memory_order_acq_rel);
            // Drop the global outstanding gate too; wake a resize possibly waiting on it reaching zero.
            if (_outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1)
                _outstanding.notify_all();
            released = true;
        }
    }
    if (released)
        _ring.lock([&](ring_state& s) { s.reclaim(*this); });
    jobs.clear();
}

void dx12_download_inline_system::on_epoch_advance(sg::epoch closed)
{
    _ring.lock(
        [&](ring_state& s)
        {
            // Snapshot the cursor as `closed`'s boundary and hand off its counter; the fresh counter
            // tallies the next epoch. reclaim in case `closed` already fully drained (counter at zero).
            s.checkpoints.push_back(epoch_checkpoint{closed, s.next_pos, cc::move(s.current_epoch_copies)});
            s.current_epoch_copies = std::make_shared<std::atomic<cc::isize>>(0);
            s.reclaim(*this);
        });
}

void dx12_download_inline_system::wait_for_submission(sg::submission_token token)
{
    if (_ctx.is_submission_complete(token))
        return;
    if (_ctx._submission_fence && _wait_event)
    {
        HRESULT const hr = _ctx._submission_fence->SetEventOnCompletion(cc::u64(token), _wait_event);
        CC_ASSERT(SUCCEEDED(hr), "ID3D12Fence::SetEventOnCompletion failed");
        WaitForSingleObject(_wait_event, INFINITE);
    }
}

void dx12_download_inline_system::on_copy_done(std::shared_ptr<std::atomic<cc::isize>> const& epoch_copies)
{
    epoch_copies->fetch_sub(1, std::memory_order_acq_rel);
    _ring.lock([&](ring_state& s) { s.reclaim(*this); });
    // Drop the global outstanding gate after the reclaim; wake a resize waiting on it reaching zero.
    if (_outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1)
        _outstanding.notify_all();
}

void dx12_download_inline_system::wait_until_idle()
{
    // Wait-for-zero on the global counter: std::atomic::wait rechecks the value, so a decrement to zero
    // between load and wait is not lost (unlike polling per-epoch checkpoints, which races the actor).
    cc::isize cur = _outstanding.load(std::memory_order_acquire);
    while (cur != 0)
    {
        _outstanding.wait(cur, std::memory_order_acquire);
        cur = _outstanding.load(std::memory_order_acquire);
    }
}

void dx12_download_inline_system::set_budget(cc::isize capacity)
{
    CC_ASSERT(capacity > 0, "download ring capacity must be positive");
    // Record the request; it is applied at the next advance_epoch (see apply_pending_budget).
    _ring.lock([&](ring_state& s) { s.pending_capacity = capacity; });
}

void dx12_download_inline_system::apply_pending_budget()
{
    cc::isize const pending = _ring.lock([](ring_state& s) { return s.pending_capacity; });
    if (pending <= 0)
        return;

    // Drain every in-flight epoch (bounds the GPU wait), then wait for the actor to finish every
    // outstanding readback copy — each memcpys out of the *current* ring, so the ring cannot be freed
    // until they are all done. The freshly-opened epoch has no downloads yet, so this reaches zero.
    while (cc::u64(_ctx.completed_epoch()) + 1 < cc::u64(_ctx.current_epoch()))
        _ctx.wait_for_next_inflight_epoch();
    _ctx.process_completed_epochs();
    wait_until_idle();

    // Build the new ring before releasing the old, so a failed allocation leaves the current one intact.
    auto ring = create_mapped_ring_buffer(_ctx._device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                                          pending);
    CC_ASSERT(ring.has_value(), "inline download ring resize failed to allocate");

    if (_buffer)
        _buffer->Unmap(0, nullptr);
    _buffer = cc::move(ring.value().resource);
    _mapped = static_cast<cc::byte*>(ring.value().mapped);
    _capacity = pending;
    _freed_pos.store(0, std::memory_order_release);
    _ring.lock(
        [&](ring_state& s)
        {
            s.next_pos = 0;
            s.checkpoints.clear();  // drained above; the fresh ring restarts its logical cursor at 0
            s.pending_capacity = 0; // current_epoch_copies stays (a fresh 0 from the last on_epoch_advance)
        });
}

dx12_download_inline_system::debug_cursor_snapshot dx12_download_inline_system::debug_cursor()
{
    cc::u64 const freed = _freed_pos.load(std::memory_order_acquire);
    return _ring.lock([&](ring_state& s) { return debug_cursor_snapshot{s.next_pos, freed, _capacity}; });
}

void dx12_download_inline_system::debug_set_cursor(cc::u64 pos)
{
    _freed_pos.store(pos, std::memory_order_release);
    _ring.lock(
        [&](ring_state& s)
        {
            s.next_pos = pos;
            s.checkpoints.clear();
        });
}

void dx12_download_inline_system::shutdown()
{
    if (_actor)
    {
        _actor->shutdown(); // drains queued copies (their GPU work is already idle by this point)
        _actor = nullptr;
    }
    if (_buffer)
    {
        _buffer->Unmap(0, nullptr);
        _buffer.Reset();
    }
    _mapped = nullptr;
    if (_wait_event)
    {
        CloseHandle(_wait_event);
        _wait_event = nullptr;
    }
}
} // namespace sg::backend::dx12
