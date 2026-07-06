// dx12_descriptor_heap: the shader-visible CBV/SRV/UAV heap, split into a per-epoch-reclaimed transient
// ring (front) and a persistent free-ranges region (rest). The transient ring reuses the u64-cursor +
// per-epoch-checkpoint scheme of the inline rings; the persistent region is a first-fit free list with
// coalescing frees. See dx12_descriptor_heap.hh and libs/graphics/shaped-graphics/docs/concepts/bindings.md.

#include <clean-core/common/assert.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_descriptor_heap.hh>

namespace sg::backend::dx12
{
cc::result<cc::unit> dx12_descriptor_heap::initialize(dx12_context& ctx, int descriptor_capacity, float transient_fraction)
{
    CC_ASSERT(descriptor_capacity > 0, "descriptor heap capacity must be positive");
    CC_ASSERT(transient_fraction >= 0.0f && transient_fraction < 1.0f, "transient fraction must be in [0, 1)");

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = UINT(descriptor_capacity);
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (HRESULT hr = ctx._device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)); FAILED(hr))
        return dx12_error(hr, "CreateDescriptorHeap (shader-visible CBV/SRV/UAV) failed");

    _ctx = &ctx;
    cpu_start = heap->GetCPUDescriptorHandleForHeapStart();
    gpu_start = heap->GetGPUDescriptorHandleForHeapStart();
    increment = int(ctx._device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
    capacity = descriptor_capacity;
    transient_capacity = int(float(descriptor_capacity) * transient_fraction);

    // The persistent region starts out as one big free span.
    persistent_free.lock([&](cc::vector<free_range>& f)
                         { f.push_back({transient_capacity, capacity - transient_capacity}); });
    return cc::unit{};
}

dx12_descriptor_alloc dx12_descriptor_heap::allocate_persistent(int count)
{
    CC_ASSERT(count > 0, "persistent descriptor allocation must be positive");
    return persistent_free.lock(
        [&](cc::vector<free_range>& f) -> dx12_descriptor_alloc
        {
            // First fit: carve `count` off the front of the first span large enough.
            for (cc::isize i = 0; i < f.size(); ++i)
            {
                if (f[i].count < count)
                    continue;
                int const offset = f[i].start;
                f[i].start += count;
                f[i].count -= count;
                if (f[i].count == 0)
                    f.remove_at(i);
                return {offset, count};
            }
            CC_UNREACHABLE("persistent descriptor region exhausted (no free span fits)");
        });
}

void dx12_descriptor_heap::free_persistent(dx12_descriptor_alloc alloc)
{
    CC_ASSERT(!alloc.is_empty(), "freeing an empty persistent descriptor range");
    int const offset = alloc.offset;
    int const count = alloc.count;
    persistent_free.lock(
        [&](cc::vector<free_range>& f)
        {
            // Append, then bubble into its sorted-by-start position (the list stays small).
            f.push_back(free_range{offset, count});
            cc::isize pos = f.size() - 1;
            while (pos > 0 && f[pos - 1].start > f[pos].start)
            {
                free_range const tmp = f[pos - 1];
                f[pos - 1] = f[pos];
                f[pos] = tmp;
                --pos;
            }

            // Coalesce with the next span, then the previous, so no adjacent free spans remain.
            if (pos + 1 < f.size() && f[pos].start + f[pos].count == f[pos + 1].start)
            {
                f[pos].count += f[pos + 1].count;
                f.remove_at(pos + 1);
            }
            if (pos > 0 && f[pos - 1].start + f[pos - 1].count == f[pos].start)
            {
                f[pos - 1].count += f[pos].count;
                f.remove_at(pos);
            }
        });
}

dx12_descriptor_alloc dx12_descriptor_heap::allocate_transient(int count)
{
    // Why a ring here and not a per-epoch bump-reset like transient *buffers*: these descriptors are
    // written by the CPU (create_binding_group) and read by the GPU during the epoch, so a slot cannot be
    // reused until the epoch that wrote it retires — resetting to 0 each epoch would let a new epoch's CPU
    // write stomp a descriptor a still-in-flight older epoch reads. Transient buffers dodge this because
    // their contents are written on the GPU timeline (FIFO-safe on the single queue), so they can bump-reset
    // and alias across epochs; descriptors cannot. The ring's checkpoints + freed_pos enforce exactly this.
    CC_ASSERT(count > 0, "transient descriptor allocation must be positive");
    CC_ASSERT(count <= transient_capacity, "a single binding group exceeds the transient descriptor region");

    for (;;)
    {
        cc::optional<int> slot = transient_ring.lock(
            [&](ring_state& s) -> cc::optional<int>
            {
                cc::u64 start = s.next_pos;
                int offset = int(start % cc::u64(transient_capacity));
                if (offset + count > transient_capacity) // a table must be contiguous: waste the tail, restart at 0
                {
                    start += cc::u64(transient_capacity - offset);
                    offset = 0;
                }
                cc::u64 const end = start + cc::u64(count);
                if (end - s.freed_pos > cc::u64(transient_capacity)) // slots still held by in-flight epochs
                    return {};
                s.next_pos = end;
                return offset; // transient region starts at heap offset 0
            });

        if (slot.has_value())
            return {slot.value(), count};

        // Ring full: retire the oldest in-flight epoch to advance the watermark. If nothing is in
        // flight, this epoch's transient groups exceed the region — a hard budget error.
        bool const any_in_flight = _ctx->_epoch_state.lock([](dx12_epoch_state& s) { return !s.in_flight.empty(); });
        CC_ASSERT(any_in_flight, "transient binding groups in one epoch exceed the transient descriptor region");
        _ctx->wait_for_next_inflight_epoch(); // retires → on_epochs_completed advances freed_pos
    }
}

void dx12_descriptor_heap::on_epoch_advance(sg::epoch closed)
{
    transient_ring.lock([&](ring_state& s) { s.checkpoints.push_back({closed, s.next_pos}); });
}

void dx12_descriptor_heap::on_epochs_completed(sg::epoch completed)
{
    transient_ring.lock(
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
} // namespace sg::backend::dx12
