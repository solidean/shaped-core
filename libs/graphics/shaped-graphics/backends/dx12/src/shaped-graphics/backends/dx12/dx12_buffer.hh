#pragma once

#include <clean-core/container/small_vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/command_list_slot.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource_access_state.hh>

namespace sg::backend::dx12
{
/// The D3D12_RESOURCE_DESC for a buffer of this shape. Shared by committed + placed creation and by a
/// memory_heap's requirement query, which must all agree on the exact desc. `size_in_bytes` must be > 0.
[[nodiscard]] D3D12_RESOURCE_DESC buffer_resource_desc(cc::isize size_in_bytes, sg::buffer_usage usage);

/// DirectX 12 implementation of sg::buffer. Holds the ID3D12Resource (GPU-resident, default heap);
/// null for an empty (size 0) buffer. For a placed buffer it also holds a handle to its backing
/// memory_heap so the heap outlives the placement.
class dx12_buffer final : public sg::buffer
{
public:
    dx12_buffer(dx12_context& ctx,
                sg::epoch created_in,
                cc::isize size_in_bytes,
                sg::buffer_usage usage,
                ComPtr<ID3D12Resource> resource,
                sg::memory_heap_handle heap = nullptr)
      : sg::buffer(size_in_bytes, usage),
        _ctx(ctx),
        _creation_epoch(created_in),
        _resource(cc::move(resource)),
        _heap(cc::move(heap))
    {
    }

    // Deferred deletion: hands the GPU handle + finalizers to the context, freed once the owning
    // epoch retires (rather than freeing here, while the GPU may still be reading it). Body in .cc.
    ~dx12_buffer() override;

    dx12_context& _ctx;                       // creating context — outlives this buffer
    sg::epoch _creation_epoch;                // epoch this buffer was created in (identity / diagnostics)
    mutable ComPtr<ID3D12Resource> _resource; // mutable: expiry releases it via a const hook
    sg::memory_heap_handle _heap;             // backing heap for a placed buffer; null when dedicated

    // --- concurrent access-state tracking ------------------------------------------------------------
    // Each open command list keys its private access state by its command_list_slot; `canonical` is the
    // committed state between lists. Mutable: a buffer's shape is fixed (shared-immutable) but its tracked
    // GPU state changes as lists record against it. Guarded by a mutex because concurrent command lists may
    // record against the same buffer. A dx12 buffer's layout is always `general` — D3D12 decays buffers to
    // COMMON at ExecuteCommandLists — so cross-list ordering rides on that decay and only intra-list
    // hazards ever produce barriers.
    struct access_slot
    {
        sg::resource_access_state state;
        bool active = false; // this slot's command list has touched the buffer since it started tracking
    };
    struct access_tracking
    {
        cc::small_vector<access_slot, 4> slots; // indexed by command_list_slot; SVO for a few concurrent lists
        sg::resource_access_state canonical;    // committed state between command lists (layout always general)
    };
    mutable cc::mutex<access_tracking> _access;

    /// Declare `stages`/`access` for `slot` (lazily starting from canonical on first touch) and return the
    /// intra-list barrier to emit before the op — `needed == false` when it is a freebie. Thread-safe.
    [[nodiscard]] sg::access_barrier declare_access(sg::command_list_slot slot,
                                                    sg::pipeline_stage_flags stages,
                                                    sg::access_flags access) const;

    /// Finalize `slot` when its command list is submitted: promote its final state to canonical if this was
    /// the last open list (`promote`), else roll back to canonical (a no-op layout-wise for buffers). Clears
    /// the slot for reuse. No barriers are needed for buffers (layout is always general). Thread-safe.
    void finalize_slot(sg::command_list_slot slot, bool promote) const;

    /// Discard `slot` when its command list is dropped: the recorded work never runs, so just clear the
    /// slot (canonical is unchanged). Thread-safe.
    void discard_slot(sg::command_list_slot slot) const;

protected:
    // Release the GPU storage (deferred to epoch retire) when the buffer is expired — see sg::buffer.
    void on_expired() const override;

private:
    // Shared by on_expired() and the destructor: stage the resource + finalizers for deferred deletion.
    // A no-op once already released (so expire()-then-destroy doesn't double-schedule).
    void release_storage() const;
};
} // namespace sg::backend::dx12
