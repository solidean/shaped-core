#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>

namespace sg::backend::dx12
{
/// The context's single shader-visible CBV/SRV/UAV descriptor heap, split into two regions by lifetime:
///
///   - a TRANSIENT ring at the front, reclaimed per epoch (a group's table frees when its epoch
///     retires), sharing the same u64-cursor + per-epoch-checkpoint scheme as the transient buffer heap;
///   - a PERSISTENT bump region for the rest, never reclaimed — a group's table lives until context
///     teardown (fine while a persistent group is held for the duration of its use).
///
/// binding_groups allocate a contiguous range from the region matching their lifetime; the command list
/// binds it as a root descriptor table. Only one shader-visible CBV/SRV/UAV heap can be bound at a time,
/// so both regions live in this one heap rather than two.
struct dx12_descriptor_heap
{
    /// Initializes the heap with `capacity` descriptors; the leading `transient_fraction` share (0..1)
    /// backs the transient ring, the rest the persistent region. `ctx` is used to retire in-flight
    /// epochs when the transient ring is full. Body in dx12_descriptor_heap.cc.
    [[nodiscard]] cc::result<cc::unit> initialize(dx12_context& ctx, UINT capacity, float transient_fraction);

    /// Bump-allocates `count` contiguous PERSISTENT descriptors; returns the heap-relative start offset.
    [[nodiscard]] UINT allocate_persistent(UINT count);

    /// Ring-allocates `count` contiguous TRANSIENT descriptors for the current epoch; returns the
    /// heap-relative start offset. Blocks (retiring in-flight epochs) when the ring is full. `count`
    /// must fit the transient region.
    [[nodiscard]] UINT allocate_transient(UINT count);

    /// Snapshots the transient ring cursor as the end-of-epoch boundary for `closed` (called at advance).
    void on_epoch_advance(sg::epoch closed);

    /// Advances the transient free watermark past every epoch <= `completed` (called at retire).
    void on_epochs_completed(sg::epoch completed);

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(UINT offset) const
    {
        return {cpu_start.ptr + SIZE_T(offset) * SIZE_T(increment)};
    }
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpu_at(UINT offset) const
    {
        return {gpu_start.ptr + UINT64(offset) * UINT64(increment)};
    }

    dx12_context* _ctx = nullptr; // for retiring epochs when the transient ring is full
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
    UINT increment = 0;
    UINT capacity = 0;
    UINT transient_capacity = 0; // [0, transient_capacity) is the ring; [transient_capacity, capacity) persistent

    std::atomic<UINT> persistent_cursor{0}; // bump offset relative to the persistent region start

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
