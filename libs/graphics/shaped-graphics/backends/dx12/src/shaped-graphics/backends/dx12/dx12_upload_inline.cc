// dx12_upload_inline_system: the inline UPLOAD ring buffer and its epoch-gated free watermark. The
// ring is a linear u64 cursor mapped onto the physical buffer via modulo; a window never wraps, but a
// copy that would straddle the seam is split at it — reserve hands back only up to the seam and the
// driver loops the resumable recorder for the remainder, so no tail is wasted. Space is reclaimed per
// epoch: at advance we snapshot the cursor for the closing epoch, and at retire we free everything up
// to the highest finished epoch.

#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_resource_upload.hh>
#include <shaped-graphics/backends/dx12/dx12_upload_inline.hh>

namespace sg::backend::dx12
{
cc::result<cc::unit> dx12_upload_inline_system::initialize(cc::isize capacity)
{
    CC_ASSERT(capacity > 0, "upload ring capacity must be positive");

    // UPLOAD heap, GENERIC_READ: the GPU reads staged bytes from here via CopyBufferRegion.
    auto ring = create_mapped_ring_buffer(_ctx._device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          capacity);
    CC_RETURN_IF_ERROR(ring);

    _buffer = cc::move(ring.value().resource);
    _mapped = static_cast<cc::byte*>(ring.value().mapped);
    _capacity = capacity;
    return cc::unit{};
}

dx12_upload_inline_system::reservation dx12_upload_inline_system::reserve(cc::isize size)
{
    CC_ASSERT(size > 0, "reserve size must be positive");
    CC_ASSERT(size <= _capacity, "a single inline upload exceeds the upload ring capacity");

    for (;;)
    {
        cc::optional<reservation> r = _ring.lock(
            [&](ring_state& s) -> cc::optional<reservation>
            {
                cc::u64 const start = s.next_pos;
                cc::isize const offset = cc::isize(start % cc::u64(_capacity));
                // Never cross the seam: grant only up to the ring end. A request that would wrap is
                // split here — the caller loops and reserves the remainder from offset 0. No tail waste.
                cc::isize const granted = cc::min(size, _capacity - offset);
                cc::u64 const end = start + cc::u64(granted);
                if (end - s.freed_pos > cc::u64(_capacity)) // space still held by in-flight epochs
                    return {};
                s.next_pos = end;
                return reservation{offset, granted};
            });

        if (r.has_value())
            return r.value();

        // Not enough free space: retire the oldest in-flight epoch to advance the watermark. If nothing
        // is in flight, this single epoch's uploads exceed the ring — a hard budget error.
        bool const any_in_flight = _ctx._epoch_state.lock([](dx12_epoch_state& s) { return !s.in_flight.empty(); });
        CC_ASSERT(any_in_flight, "inline uploads in one epoch exceed the upload ring capacity");
        _ctx.wait_for_next_inflight_epoch(); // retires → on_epochs_completed advances freed_pos
    }
}

void dx12_upload_inline_system::upload_buffer(dx12_command_list& cmd,
                                              dx12_buffer const& dst,
                                              cc::span<cc::byte const> data,
                                              cc::isize dst_offset)
{
    if (data.empty())
        return;

    CC_ASSERT(_mapped != nullptr, "upload system used before initialization");

    dx12_buffer_upload upload(dst, dst_offset, data);
    upload.prepare(cmd);

    // One pass per contiguous ring slice: a buffer that fits without wrapping is a single job; one that
    // straddles the seam (or a chunked texture later) is split across successive reservations.
    while (!upload.is_finished())
    {
        cc::isize const remaining = upload.total_bytes() - upload.consumed();
        reservation const res = reserve(remaining);
        dx12_upload_allocation const alloc{_buffer.Get(), _mapped, res.offset, res.granted};
        cc::isize const consumed = upload.execute_next_job(*cmd._list.Get(), alloc);
        CC_ASSERT(consumed > 0, "inline upload made no progress");
    }
}

void dx12_upload_inline_system::on_epoch_advance(sg::epoch closed)
{
    _ring.lock([&](ring_state& s) { s.checkpoints.push_back({closed, s.next_pos}); });
}

void dx12_upload_inline_system::on_epochs_completed(sg::epoch completed)
{
    _ring.lock(
        [&](ring_state& s)
        {
            cc::isize retired = 0;
            for (auto const& cp : s.checkpoints)
            {
                if (cc::u64(cp.epoch_id) > cc::u64(completed))
                    break;
                s.freed_pos = cp.end_pos; // checkpoints are monotonic in epoch and end_pos
                ++retired;
            }
            s.checkpoints.remove_from_to(0, retired);
        });
}

void dx12_upload_inline_system::set_budget(cc::isize capacity)
{
    CC_ASSERT(capacity > 0, "upload ring capacity must be positive");
    // Record the request; it is applied at the next advance_epoch (see apply_pending_budget).
    _ring.lock([&](ring_state& s) { s.pending_capacity = capacity; });
}

void dx12_upload_inline_system::apply_pending_budget()
{
    cc::isize const pending = _ring.lock([](ring_state& s) { return s.pending_capacity; });
    if (pending <= 0)
        return;

    // Drain every in-flight epoch so no GPU work still reads the ring, then retire them so their ring
    // space is reclaimed. After this the ring is empty (only the freshly-opened epoch remains, with no
    // uploads yet), so it is safe to drop and rebuild.
    while (cc::u64(_ctx.completed_epoch()) + 1 < cc::u64(_ctx.current_epoch()))
        _ctx.wait_for_next_inflight_epoch();
    _ctx.process_completed_epochs();

    // Build the new ring before releasing the old, so a failed allocation leaves the current one intact.
    auto ring = create_mapped_ring_buffer(_ctx._device.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          pending);
    CC_ASSERT(ring.has_value(), "inline upload ring resize failed to allocate");

    if (_buffer)
        _buffer->Unmap(0, nullptr);
    _buffer = cc::move(ring.value().resource);
    _mapped = static_cast<cc::byte*>(ring.value().mapped);
    _capacity = pending;
    _ring.lock(
        [&](ring_state& s)
        {
            s.next_pos = 0;
            s.freed_pos = 0;
            s.checkpoints.clear(); // drained above; the fresh ring restarts its logical cursor at 0
            s.pending_capacity = 0;
        });
}

dx12_upload_inline_system::debug_cursor_snapshot dx12_upload_inline_system::debug_cursor()
{
    return _ring.lock([&](ring_state& s) { return debug_cursor_snapshot{s.next_pos, s.freed_pos, _capacity}; });
}

void dx12_upload_inline_system::debug_set_cursor(cc::u64 pos)
{
    _ring.lock(
        [&](ring_state& s)
        {
            s.next_pos = pos;
            s.freed_pos = pos;
            s.checkpoints.clear();
        });
}

void dx12_upload_inline_system::shutdown()
{
    if (_buffer)
    {
        _buffer->Unmap(0, nullptr);
        _buffer.Reset();
    }
    _mapped = nullptr;
}
} // namespace sg::backend::dx12
