#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::backend::dx12
{
/// The context's single shader-visible CBV/SRV/UAV descriptor heap, split into two regions by lifetime.
/// Only one shader-visible CBV/SRV/UAV heap can be bound at a time, so both regions live in this one heap.
///
/// The two regions use deliberately different allocators, because their hazard models differ:
///
///   - TRANSIENT (leading fraction): a **ring**, reclaimed per epoch. Descriptors are written by the CPU
///     when a group is created and read by the GPU during that epoch, so a slot cannot be reused until
///     the epoch that wrote it has retired (a CPU/GPU in-flight hazard). The ring's per-epoch checkpoints
///     enforce exactly that. (This is unlike the transient *buffer* heap, whose contents are only ever
///     GPU-touched: a serialized queue lets it use a plain bump-reset with cross-epoch aliasing.)
///
///   - PERSISTENT (the rest): a **free-ranges allocator** — a group's range is returned to the free list
///     when the group is released, deferred until its last-using epoch retires (a finalizer on the
///     epoch, mirroring buffer deletion). So long-lived groups don't leak the heap.
///
/// A binding_group allocates a contiguous range from the region matching its lifetime; the command list
/// binds it as a root descriptor table.
struct dx12_descriptor_heap
{
    /// Initializes the heap with `capacity` descriptors; the leading `transient_fraction` share (0..1)
    /// backs the transient ring, the rest the persistent free list. `ctx` is used to retire in-flight
    /// epochs when the transient ring is full. Body in dx12_descriptor_heap.cc.
    [[nodiscard]] cc::result<cc::unit> initialize(dx12_context& ctx, int capacity, float transient_fraction);

    /// Allocates `count` contiguous PERSISTENT descriptors from the free list; returns the heap-relative
    /// start offset. Free it with free_persistent when the group is released.
    [[nodiscard]] int allocate_persistent(int count);

    /// Returns a persistent range (as from allocate_persistent) to the free list, coalescing neighbours.
    void free_persistent(int offset, int count);

    /// Ring-allocates `count` contiguous TRANSIENT descriptors for the current epoch; returns the
    /// heap-relative start offset. Blocks (retiring in-flight epochs) when the ring is full. `count`
    /// must fit the transient region.
    [[nodiscard]] int allocate_transient(int count);

    /// Snapshots the transient ring cursor as the end-of-epoch boundary for `closed` (called at advance).
    void on_epoch_advance(sg::epoch closed);

    /// Advances the transient free watermark past every epoch <= `completed` (called at retire).
    void on_epochs_completed(sg::epoch completed);

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(int offset) const
    {
        return {cpu_start.ptr + SIZE_T(offset) * SIZE_T(increment)};
    }
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpu_at(int offset) const
    {
        return {gpu_start.ptr + UINT64(offset) * UINT64(increment)};
    }

    dx12_context* _ctx = nullptr; // for retiring epochs when the transient ring is full
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
    int increment = 0;
    int capacity = 0;
    int transient_capacity = 0; // [0, transient_capacity) is the ring; [transient_capacity, capacity) persistent

    /// A free span [start, start+count) of persistent descriptors (heap-relative). The list is kept
    /// sorted by start with no adjacent spans (freeing coalesces).
    struct free_range
    {
        int start = 0;
        int count = 0;
    };
    cc::mutex<cc::vector<free_range>> persistent_free;

    /// A logical end-cursor snapshot for a closed epoch; its transient slots free once the epoch retires.
    struct epoch_checkpoint
    {
        sg::epoch epoch_id = sg::epoch::invalid;
        cc::u64 end_pos = 0;
    };
    struct ring_state
    {
        cc::u64 next_pos = 0;                     // logical bump cursor over the transient region
        cc::u64 freed_pos = 0;                    // everything logically below this is reclaimable
        cc::vector<epoch_checkpoint> checkpoints; // FIFO, oldest epoch at the front
    };
    cc::mutex<ring_state> transient_ring;
};
} // namespace sg::backend::dx12
