// dx12_descriptor_heap: the shader-visible CBV/SRV/UAV heap, split into a per-epoch-reclaimed transient
// ring (front) and a persistent bump region (rest). The transient ring reuses the u64-cursor +
// per-epoch-checkpoint scheme of the inline rings; the persistent region is a plain atomic bump. See
// libs/graphics/shaped-graphics/docs/concepts/bindings.md.

#include <clean-core/common/assert.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_descriptor_heap.hh>

namespace sg::backend::dx12
{
cc::result<cc::unit> dx12_descriptor_heap::initialize(dx12_context& ctx, UINT descriptor_capacity, float transient_fraction)
{
    CC_ASSERT(descriptor_capacity > 0, "descriptor heap capacity must be positive");
    CC_ASSERT(transient_fraction >= 0.0f && transient_fraction < 1.0f, "transient fraction must be in [0, 1)");

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = descriptor_capacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (HRESULT hr = ctx._device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)); FAILED(hr))
        return dx12_error(hr, "CreateDescriptorHeap (shader-visible CBV/SRV/UAV) failed");

    _ctx = &ctx;
    cpu_start = heap->GetCPUDescriptorHandleForHeapStart();
    gpu_start = heap->GetGPUDescriptorHandleForHeapStart();
    increment = ctx._device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    capacity = descriptor_capacity;
    transient_capacity = UINT(float(descriptor_capacity) * transient_fraction);
    return cc::unit{};
}

UINT dx12_descriptor_heap::allocate_persistent(UINT count)
{
    UINT const offset = persistent_cursor.fetch_add(count, std::memory_order_relaxed);
    CC_ASSERT(transient_capacity + offset + count <= capacity, "persistent descriptor region exhausted (bump "
                                                               "allocator, no reclaim)");
    return transient_capacity + offset;
}

UINT dx12_descriptor_heap::allocate_transient(UINT count)
{
    CC_ASSERT(count > 0, "transient descriptor allocation must be positive");
    CC_ASSERT(count <= transient_capacity, "a single binding group exceeds the transient descriptor region");

    for (;;)
    {
        cc::optional<UINT> slot = transient_ring.lock(
            [&](ring_state& s) -> cc::optional<UINT>
            {
                cc::u64 start = s.next_pos;
                UINT offset = UINT(start % cc::u64(transient_capacity));
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
            return slot.value();

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
