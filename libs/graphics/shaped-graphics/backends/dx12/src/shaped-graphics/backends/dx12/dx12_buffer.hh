#pragma once

#include <clean-core/container/small_vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backend/command_list_slot.hh>
#include <shaped-graphics/backend/resource_access_state.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raw_buffer.hh>

#include <atomic>

namespace sg::backend::dx12
{
/// The D3D12_RESOURCE_DESC for a buffer of this shape. Shared by committed + placed creation and by a
/// memory_heap's requirement query, which must all agree on the exact desc. `size_in_bytes` must be > 0.
[[nodiscard]] D3D12_RESOURCE_DESC buffer_resource_desc(cc::isize size_in_bytes, sg::buffer_usage usage);

/// DirectX 12 implementation of sg::raw_buffer. Holds the ID3D12Resource (GPU-resident, default heap);
/// null for an empty (size 0) buffer. For a placed buffer it also holds a handle to its backing
/// memory_heap so the heap outlives the placement.
class dx12_buffer final : public sg::raw_buffer
{
public:
    dx12_buffer(dx12_context& ctx,
                sg::epoch created_in,
                cc::isize size_in_bytes,
                sg::buffer_usage usage,
                ComPtr<ID3D12Resource> resource,
                sg::memory_heap_handle heap = nullptr)
      : sg::raw_buffer(size_in_bytes, usage),
        _ctx(ctx),
        _creation_epoch(created_in),
        _resource(cc::move(resource)),
        _heap(cc::move(heap))
    {
    }

    // Deferred deletion: hands the GPU handle + finalizers to the context, freed once the owning
    // epoch retires (rather than freeing here, while the GPU may still be reading it). Body in .cc.
    ~dx12_buffer() override;

    /// The buffer's GPU virtual address (0 for an empty / size-0 buffer, whose _resource is null). Used by
    /// the raytracing build path, which references vertices / scratch / result by address.
    [[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS gpu_virtual_address() const
    {
        return _resource ? _resource->GetGPUVirtualAddress() : D3D12_GPU_VIRTUAL_ADDRESS(0);
    }

    dx12_context& _ctx;                       // creating context — outlives this buffer
    sg::epoch _creation_epoch;                // epoch this buffer was created in (identity / diagnostics)
    mutable ComPtr<ID3D12Resource> _resource; // mutable: expiry releases it via a const hook
    sg::memory_heap_handle _heap;             // backing heap for a placed buffer; null when dedicated

    // Two per-resource cross-queue sync stamps that make the CPU timeline (submit → async upload → submit)
    // mirror GPU ordering between the direct queue and the async-upload copy queue. Both only ever grow, are
    // never reset (a stale value just yields a cheap already-satisfied wait), and are mutable+atomic (stamped
    // through the const buffer handle from any thread). Distinct from the access-state tracking below: that
    // orders direct-queue lists against each other; these order the copy queue against the direct queue.

    // Forward: highest completion value an ASYNC upload (ctx.upload, not the inline cmd.upload) here will
    // signal on the copy queue. A later direct-queue list that reads this buffer waits for it at submit,
    // so it sees the async write.
    mutable std::atomic<cc::u64> _pending_async_upload_value = 0;

    // Reverse: highest direct-queue submission token of a command list that used this buffer. An async
    // upload here defers its copy until this token completes, so it never overwrites the buffer while an
    // earlier-submitted list still uses it. The async download (ctx.download) reuses it too: its readback
    // defers behind this token so it reads the buffer only after the last direct-queue writer has finished.
    mutable std::atomic<cc::u64> _last_used_submission_token = 0;

    // Forward for the async DOWNLOAD (ctx.download): highest completion value a pending async readback here
    // will signal on the download completion fence (dx12_download_async_system::_completion_fence) once its
    // copy-queue read has finished. A later direct-queue list that WRITES this buffer waits for it at
    // submit, so it never overwrites bytes the readback is still reading. `0` == no pending async download.
    mutable std::atomic<cc::u64> _pending_async_download_value = 0;

    // --- concurrent access-state tracking ------------------------------------------------------------
    // Each open command list keys its private intra-list access state by its command_list_slot. Guarded by a
    // mutex because concurrent command lists may record against the same buffer. Unlike a texture there is no
    // canonical between-lists state and no promote/revert: a dx12 buffer's layout is always `general` (D3D12
    // decays buffers to COMMON at ExecuteCommandLists), so cross-list ordering rides on that decay and only
    // *intra-list* hazards ever produce a barrier — each list seeds a fresh state and clears its slot at
    // submit/drop.
    struct access_slot
    {
        sg::resource_access_state state;
        bool active = false;          // this slot's command list has touched the buffer since it started tracking
        bool recorded = false;        // this slot's command list has added the buffer to its finalize set (dedup)
        bool pending_barrier = false; // declared for the current op, awaiting the pre-op flush (per-op dedup)
    };
    struct access_tracking
    {
        cc::small_vector<access_slot, 4> slots; // indexed by command_list_slot; SVO for a few concurrent lists
    };
    mutable cc::mutex<access_tracking> _access;

    /// Accumulate one declared `stages`/`access` for `slot` (a fresh state on first touch) into the next-op
    /// state, without emitting anything. Call once per binding — a buffer bound several times to the same op
    /// declares several times; `flush_access` then merges them into one barrier. Thread-safe.
    void declare_access(sg::command_list_slot slot, sg::pipeline_stage_flags stages, sg::access_flags access) const;

    /// Test-and-set `slot`'s pending-barrier flag: true the first time it is called for `slot` since the last
    /// flush, false after. The command list uses it to enqueue the buffer for the pre-op barrier flush
    /// exactly once, no matter how many times it is bound. `flush_access` clears it. Thread-safe.
    [[nodiscard]] bool mark_pending_barrier(sg::command_list_slot slot) const;

    /// Flush the accesses declared for `slot` since the last flush and return the single intra-list barrier
    /// that satisfies their union — `needed == false` when it is a freebie. Called once per op, before it,
    /// after all its bindings are declared; also clears the pending-barrier flag. Thread-safe.
    [[nodiscard]] sg::access_barrier flush_access(sg::command_list_slot slot) const;

    /// Test-and-set `slot`'s finalize-recorded flag: true the first time it is called for `slot`, false
    /// after (until the slot is cleared by finalize/discard). The command list uses it to add the buffer to
    /// its touched set exactly once, in O(1) — replacing a linear scan. Thread-safe.
    [[nodiscard]] bool mark_recorded(sg::command_list_slot slot) const;

    /// Finalize `slot` when its command list is submitted: just clear the slot (frees the tracking slot for a
    /// future list; a buffer has no layout to promote or revert, and no between-lists state to carry). No
    /// barriers are needed for buffers. Thread-safe.
    void finalize_slot(sg::command_list_slot slot) const;

    /// Discard `slot` when its command list is dropped: the recorded work never runs, so just clear the slot.
    /// Identical to `finalize_slot` for a buffer (nothing to promote or revert). Thread-safe.
    void discard_slot(sg::command_list_slot slot) const;

protected:
    // Release the GPU storage (deferred to epoch retire) when the buffer is expired — see sg::raw_buffer.
    void on_expired() const override;

private:
    // Shared by on_expired() and the destructor: stage the resource + finalizers for deferred deletion.
    // A no-op once already released (so expire()-then-destroy doesn't double-schedule).
    void release_storage() const;
};
} // namespace sg::backend::dx12
