// dx12_transient_buffer_allocator: the transient BUFFER ring and its epoch-gated free watermark. The
// ring is a linear u64 cursor mapped onto the heap via modulo; windows never wrap (a would-be wrap
// wastes the tail and restarts at 0). Space is reclaimed per epoch: at advance we snapshot the cursor
// for the closing epoch, at retire we free everything up to the highest finished epoch. See
// libs/graphics/shaped-graphics/docs/concepts/memory.md.

#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_memory_heap.hh>
#include <shaped-graphics/backends/dx12/dx12_transient_buffer_allocator.hh>

namespace sg::backend::dx12
{
namespace
{
cc::isize align_up(cc::isize value, cc::isize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

cc::result<cc::unit> dx12_transient_buffer_allocator::initialize(cc::isize capacity)
{
    CC_ASSERT(capacity > 0, "transient heap capacity must be positive");

    // Placement offsets (and therefore the ring positions) must be a multiple of the buffer placement
    // alignment; keep the whole ring on that grid by aligning the capacity up.
    cc::isize const aligned = align_up(capacity, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    auto heap = dx12_memory_heap::create(_ctx._device.Get(), aligned);
    CC_RETURN_IF_ERROR(heap);

    _heap = heap.value();
    _capacity = _heap->size_in_bytes();
    return cc::unit{};
}

cc::isize dx12_transient_buffer_allocator::reserve(cc::isize size)
{
    CC_ASSERT(size > 0, "reserve size must be positive");
    CC_ASSERT(size <= _capacity, "a single transient buffer exceeds the transient heap capacity");

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
        // is in flight, this single epoch's transient buffers exceed the heap — a hard budget error.
        bool const any_in_flight = _ctx._epoch_state.lock([](dx12_epoch_state& s) { return !s.in_flight.empty(); });
        CC_ASSERT(any_in_flight, "transient buffers in one epoch exceed the transient heap capacity");
        _ctx.wait_for_next_inflight_epoch(); // retires → on_epochs_completed advances freed_pos
    }
}

cc::result<dx12_buffer_handle> dx12_transient_buffer_allocator::create_buffer(cc::isize size_in_bytes,
                                                                             sg::buffer_usage usage)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");

    // Empty buffer: no placement; a dedicated empty buffer still carries the transient scope.
    if (size_in_bytes == 0)
    {
        sg::allocation_info info;
        info.scope = sg::lifetime_scope::transient;
        return _ctx.create_dx12_buffer(0, usage, info);
    }

    sg::memory_requirements const reqs = _heap->memory_requirements_for_buffer(size_in_bytes, usage);

    // Larger than the whole ring: committed (dedicated) fallback, still transient.
    if (reqs.size_in_bytes > _capacity)
    {
        sg::allocation_info info;
        info.scope = sg::lifetime_scope::transient;
        return _ctx.create_dx12_buffer(size_in_bytes, usage, info);
    }

    cc::isize const offset = reserve(reqs.size_in_bytes);
    sg::allocation_info info = _heap->acquire_allocation_for_buffer(size_in_bytes, usage, offset);
    info.scope = sg::lifetime_scope::transient;
    return _ctx.create_dx12_buffer(size_in_bytes, usage, info);
}

void dx12_transient_buffer_allocator::on_epoch_advance(sg::epoch closed)
{
    _ring.lock([&](ring_state& s) { s.checkpoints.push_back({closed, s.next_pos}); });
}

void dx12_transient_buffer_allocator::on_epochs_completed(sg::epoch completed)
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

void dx12_transient_buffer_allocator::shutdown()
{
    _heap = nullptr;
    _capacity = 0;
}

cc::result<dx12_buffer_handle> dx12_context::create_dx12_transient_buffer(cc::isize size_in_bytes,
                                                                          sg::buffer_usage usage)
{
    return _transient_buffers.create_buffer(size_in_bytes, usage);
}
} // namespace sg::backend::dx12
