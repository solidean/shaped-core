#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/backends/dx12/dx12_command_allocator_pool.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>

// Per-epoch bookkeeping structs for the dx12 backend's epoch system. The epoch *concept* lives in
// sg:: (fwd.hh + the sg::context contract); this is dx12's concrete realization. See
// libs/graphics/shaped-graphics/docs/concepts/epochs.md.

namespace sg::backend::dx12
{
/// A GPU resource captured for deferred deletion. Its handle is released and its finalizers run only
/// once the GPU is no longer using it: both the owning epoch has retired (direct queue) *and* the async
/// upload copy queue has passed `copy_wait`. Future work hangs more here: pipeline/state-object handles
/// and descriptor allocations.
struct dx12_expiring_resource
{
    ComPtr<ID3D12Resource> resource;
    cc::vector<cc::unique_function<void()>> finalizers;
    /// Copy-queue completion value that must be reached before release — the buffer's highest pending
    /// async-upload value (`dx12_buffer::_pending_async_upload_value`). `none` (0) when no async upload
    /// ever targeted it, so the copy gate is trivially satisfied.
    dx12_copy_fence_value copy_wait = dx12_copy_fence_value::none;
};

/// Everything one epoch owns and must reclaim once its GPU work finishes. Built at advance, drained
/// at retire. Future per-epoch fields (staging byte counts, transient descriptor counts, group
/// fences, RTV/DSV frees) attach here.
struct dx12_epoch_data
{
    epoch epoch_id = epoch::invalid;
    cc::vector<dx12_pooled_allocator> allocators; // reset + returned to the pool on retire
    cc::vector<dx12_expiring_resource> expiring;  // freed on retire
};

/// The mutex-guarded epoch state: the in-flight FIFO plus the deferred-deletion staging area.
/// Guarded because a resource's refcount can hit zero (staging a deletion) on any thread, while
/// advance/retire run on the driver thread.
struct dx12_epoch_state
{
    cc::vector<dx12_epoch_data> in_flight;     // FIFO, oldest at the front
    cc::vector<dx12_expiring_resource> staged; // refcount-zero resources awaiting the next advance
    // Resources whose epoch has retired but whose async copy-queue work (`copy_wait`) is still pending.
    // Re-checked each process_completed_epochs sweep; released once the copy fence catches up. Normally
    // empty and short-lived — the copy queue drains promptly behind the direct queue.
    cc::vector<dx12_expiring_resource> copy_deferred;
};

/// Reclaims one expiring resource in the required order: null the GPU handle *first* (releasing GPU
/// memory), then move its finalizers into `out_finalizers` to be run once the caller has left any
/// lock. A finalizer must never observe a resource whose handle is still live.
inline void release_expiring(dx12_expiring_resource& r, cc::vector<cc::unique_function<void()>>& out_finalizers)
{
    r.resource.Reset();
    for (auto& f : r.finalizers)
        out_finalizers.push_back(cc::move(f));
    r.finalizers.clear();
}
} // namespace sg::backend::dx12
