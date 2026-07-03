// dx12_upload_inline_system: the inline UPLOAD ring buffer and its epoch-gated free watermark. The
// ring is a linear u64 cursor mapped onto the physical buffer via modulo; windows never wrap (a would-
// be wrap wastes the tail and restarts at 0). Space is reclaimed per epoch: at advance we snapshot the
// cursor for the closing epoch, and at retire we free everything up to the highest finished epoch.

#include <clean-core/error/optional.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_resource_upload.hh>
#include <shaped-graphics/backends/dx12/dx12_upload_inline.hh>

namespace sg::backend::dx12
{
void dx12_upload_inline_system::initialize(ComPtr<ID3D12Resource> buffer, cc::byte* mapped, cc::isize capacity)
{
    CC_ASSERT(capacity > 0, "upload ring capacity must be positive");
    _buffer = cc::move(buffer);
    _mapped = mapped;
    _capacity = capacity;
}

cc::isize dx12_upload_inline_system::reserve(cc::isize size)
{
    CC_ASSERT(size > 0, "reserve size must be positive");
    CC_ASSERT(size <= _capacity, "a single inline upload exceeds the upload ring capacity");

    for (;;)
    {
        cc::optional<cc::isize> phys = _ring.lock(
            [&](ring_state& s) -> cc::optional<cc::isize>
            {
                cc::u64 start = s.next_pos;
                cc::isize offset = cc::isize(start % cc::u64(_capacity));
                if (offset + size > _capacity) // would wrap: waste the tail, restart at 0
                {
                    start += cc::u64(_capacity - offset);
                    offset = 0;
                }
                cc::u64 const end = start + cc::u64(size);
                if (end - s.freed_pos > cc::u64(_capacity)) // space still held by in-flight epochs
                    return {};
                s.next_pos = end;
                return offset;
            });

        if (phys.has_value())
            return phys.value();

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

    // Buffers stage in one job; the loop is here for chunked resources (textures) later.
    while (!upload.is_finished())
    {
        cc::isize const remaining = upload.total_bytes(); // single job for buffers
        cc::isize const offset = reserve(remaining);
        dx12_upload_allocation const alloc{_buffer.Get(), _mapped, offset, remaining};
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
