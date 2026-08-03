#pragma once

#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/small_vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_access.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/raw_texture.hh>

namespace sg::backend::dx12
{
/// The D3D12_RESOURCE_DESC for a texture of this shape.
/// The create path is its only caller today: placed textures and heap requirement queries need a texture-capable memory_heap, which dx12 does not have.
[[nodiscard]] D3D12_RESOURCE_DESC texture_resource_desc(sg::texture_description const& desc);

/// DirectX 12 implementation of sg::raw_texture, holding the ID3D12Resource (GPU-resident, default heap).
/// Also owns the per-command-list subresource access tracking that drives layout-transition barriers — see dx12_texture_access.
class dx12_texture final : public sg::raw_texture
{
public:
    dx12_texture(dx12_context& ctx,
                 sg::epoch created_in,
                 sg::texture_description const& desc,
                 ComPtr<ID3D12Resource> resource,
                 sg::memory_heap_handle heap = nullptr,
                 bool borrowed = false)
      : sg::raw_texture(desc),
        _ctx(ctx),
        _creation_epoch(created_in),
        _resource(cc::move(resource)),
        _heap(cc::move(heap)),
        _borrowed(borrowed),
        _access(subresource_extent_of(desc))
    {
    }

    // Deferred deletion: hands the GPU handle + finalizers to the context, freed once the owning epoch retires.
    // Freeing here would race the GPU still reading it.
    ~dx12_texture() override;

    dx12_context& _ctx;                       // creating context — outlives this texture
    sg::epoch _creation_epoch;                // epoch this texture was created in (identity / diagnostics)
    mutable ComPtr<ID3D12Resource> _resource; // mutable: expiry releases it via a const hook
    sg::memory_heap_handle _heap;             // backing heap for a placed texture; null when dedicated

    // Borrowed (externally-owned) storage — a swapchain back buffer owned by DXGI, not us.
    // Released synchronously, since the swapchain waits for the GPU before dropping it, rather than through the epoch's deferred deletion.
    // ResizeBuffers and teardown then see zero outstanding references immediately.
    bool _borrowed = false;

    // Cross-queue sync stamps mirroring dx12_buffer's: they order the async copy queues (ctx.upload / ctx.download) against the direct queue.
    // They only grow, are never reset, and are mutable + atomic.
    // An async texture copy requires the texture in the COMMON layout on the copy queue — a freshly created texture qualifies, one already in a shader/attachment layout must not be async-copied.
    // See libs/graphics/shaped-graphics/docs/concepts/upload.async.md.
    mutable std::atomic<u64> _pending_async_upload_value = 0;   // forward: a later reader waits at submit
    mutable std::atomic<u64> _last_used_submission_token = 0;   // reverse: an async copy defers behind it
    mutable std::atomic<u64> _pending_async_download_value = 0; // forward: a later writer waits at submit

    // Per-command-list subresource access tracking.
    // Mutable: a texture's shape is fixed, but its tracked GPU state changes as lists record against it.
    // Guarded because concurrent lists may record the same texture.
    // The thin forwarders below return the barriers the command list must emit; dx12 owns barrier tracking and emission.
    mutable cc::mutex<dx12_texture_access> _access;

    /// Accumulate one declared `stages`/`access`/`layout` over `range` for `slot` into the next-op state, emitting no barrier.
    /// Call once per binding; `flush_texture_access` merges them.
    /// Thread-safe.
    void declare_texture_access(sg::command_list_slot slot,
                                sg::subresource_range range,
                                sg::pipeline_stage_flags stages,
                                sg::access_flags access,
                                sg::texture_layout layout) const
    {
        _access.lock([&](dx12_texture_access& t) { t.declare(slot, range, stages, access, layout); });
    }

    /// Test-and-set `slot`'s pending-barrier flag — see dx12_texture_access::mark_pending_barrier.
    /// True on the first binding this op, false after; `flush_texture_access` clears it.
    /// Thread-safe.
    [[nodiscard]] bool mark_pending_barrier(sg::command_list_slot slot) const
    {
        return _access.lock([&](dx12_texture_access& t) { return t.mark_pending_barrier(slot); });
    }

    /// Flush the accesses declared for `slot` since the last flush and return the per-box barriers to emit before the op; empty means all freebies.
    /// Merges multiple declares of the same box.
    /// Thread-safe.
    [[nodiscard]] cc::small_vector<dx12_subresource_barrier, 4> flush_texture_access(sg::command_list_slot slot) const
    {
        return _access.lock([&](dx12_texture_access& t) { return t.flush(slot); });
    }

    /// Finalize `slot` at submit: promote its final layout to the canonical state if this was the last list touching the texture, else revert it to the canonical layout.
    /// Returns the transitions to emit.
    /// Thread-safe.
    [[nodiscard]] cc::small_vector<dx12_subresource_barrier, 4> finalize_slot(sg::command_list_slot slot) const
    {
        return _access.lock([&](dx12_texture_access& t) { return t.finalize(slot); });
    }

    /// Discard `slot` at drop: the recorded work never runs, so just clear the slot.
    /// Thread-safe.
    void discard_slot(sg::command_list_slot slot) const
    {
        _access.lock([&](dx12_texture_access& t) { t.discard(slot); });
    }

    /// Test-and-set `slot`'s finalize-recorded flag — see dx12_texture_access::mark_recorded.
    /// True the first time per slot, false after, so the command list records the texture for finalization once.
    /// Thread-safe.
    [[nodiscard]] bool mark_recorded(sg::command_list_slot slot) const
    {
        return _access.lock([&](dx12_texture_access& t) { return t.mark_recorded(slot); });
    }

protected:
    // Release the GPU storage, deferred to epoch retire, when the texture is expired — see sg::raw_texture.
    void on_expired() const override;

private:
    // Shared by on_expired() and the destructor: stage the resource + finalizers for deferred deletion.
    // A no-op once already released, so expire()-then-destroy does not double-schedule.
    void release_storage() const;
};
} // namespace sg::backend::dx12
