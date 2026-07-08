#pragma once

#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/small_vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_access.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raw_texture.hh>

namespace sg::backend::dx12
{
/// The D3D12_RESOURCE_DESC for a texture of this shape. Shared by the create path (and, later, placed
/// creation + heap requirement queries) so they all agree on the exact desc.
[[nodiscard]] D3D12_RESOURCE_DESC texture_resource_desc(sg::texture_description const& desc);

/// DirectX 12 implementation of sg::raw_texture. Holds the ID3D12Resource (GPU-resident, default heap).
/// For a placed texture it also holds a handle to its backing memory_heap so the heap outlives it. Also
/// owns the per-command-list subresource access tracking that drives layout-transition barriers (see
/// dx12_texture_access) — no public op records against a texture yet, so the tracking is wired + tested
/// but not driven end-to-end.
class dx12_texture final : public sg::raw_texture
{
public:
    dx12_texture(dx12_context& ctx,
                 sg::epoch created_in,
                 sg::texture_description const& desc,
                 ComPtr<ID3D12Resource> resource,
                 sg::memory_heap_handle heap = nullptr)
      : sg::raw_texture(desc),
        _ctx(ctx),
        _creation_epoch(created_in),
        _resource(cc::move(resource)),
        _heap(cc::move(heap)),
        _access(subresource_extent_of(desc))
    {
    }

    // Deferred deletion: hands the GPU handle + finalizers to the context, freed once the owning epoch
    // retires (rather than freeing here, while the GPU may still be reading it). Body in .cc.
    ~dx12_texture() override;

    dx12_context& _ctx;                       // creating context — outlives this texture
    sg::epoch _creation_epoch;                // epoch this texture was created in (identity / diagnostics)
    mutable ComPtr<ID3D12Resource> _resource; // mutable: expiry releases it via a const hook
    sg::memory_heap_handle _heap;             // backing heap for a placed texture; null when dedicated

    // Per-command-list subresource access tracking. Mutable: a texture's shape is fixed but its tracked GPU
    // state changes as lists record against it; guarded because concurrent lists may record the same texture.
    // Thin forwarders return the barriers the command list must emit (dx12 owns barrier tracking + emission).
    mutable cc::mutex<dx12_texture_access> _access;

    /// Declare `stages`/`access`/`layout` over `range` for `slot` and return the intra-list barriers to
    /// emit before the op (empty = all freebies). Thread-safe.
    [[nodiscard]] cc::small_vector<dx12_subresource_barrier, 4> declare_texture_access(sg::command_list_slot slot,
                                                                                       sg::subresource_range range,
                                                                                       sg::pipeline_stage_flags stages,
                                                                                       sg::access_flags access,
                                                                                       sg::texture_layout layout) const
    {
        return _access.lock([&](dx12_texture_access& t) { return t.declare(slot, range, stages, access, layout); });
    }

    /// Finalize `slot` at submit: promote its final layout to the committed state (last open list) or revert
    /// the texture to its entry layout, returning the transitions to emit. Thread-safe.
    [[nodiscard]] cc::small_vector<dx12_subresource_barrier, 4> finalize_slot(sg::command_list_slot slot, bool promote) const
    {
        return _access.lock([&](dx12_texture_access& t) { return t.finalize(slot, promote); });
    }

    /// Discard `slot` at drop: the recorded work never runs, so just clear the slot. Thread-safe.
    void discard_slot(sg::command_list_slot slot) const
    {
        _access.lock([&](dx12_texture_access& t) { t.discard(slot); });
    }

protected:
    // Release the GPU storage (deferred to epoch retire) when the texture is expired — see sg::raw_texture.
    void on_expired() const override;

private:
    // Shared by on_expired() and the destructor: stage the resource + finalizers for deferred deletion.
    // A no-op once already released (so expire()-then-destroy doesn't double-schedule).
    void release_storage() const;
};
} // namespace sg::backend::dx12
