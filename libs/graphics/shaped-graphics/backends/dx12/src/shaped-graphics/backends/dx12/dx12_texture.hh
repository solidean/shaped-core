#pragma once

#include <clean-core/common/utility.hh> // cc::move
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raw_texture.hh>

namespace sg::backend::dx12
{
/// The D3D12_RESOURCE_DESC for a texture of this shape. Shared by the create path (and, later, placed
/// creation + heap requirement queries) so they all agree on the exact desc.
[[nodiscard]] D3D12_RESOURCE_DESC texture_resource_desc(sg::texture_description const& desc);

/// DirectX 12 implementation of sg::raw_texture. Holds the ID3D12Resource (GPU-resident, default heap).
/// For a placed texture it also holds a handle to its backing memory_heap so the heap outlives it.
///
/// No per-command-list access-state tracking yet (unlike dx12_buffer): a texture is creatable but not
/// usable in command lists until layout transitions land — see
/// libs/graphics/shaped-graphics/docs/concepts/textures.md.
class dx12_texture final : public sg::raw_texture
{
public:
    dx12_texture(dx12_context& ctx,
                 sg::epoch created_in,
                 sg::texture_description const& desc,
                 ComPtr<ID3D12Resource> resource,
                 sg::memory_heap_handle heap = nullptr)
      : sg::raw_texture(desc), _ctx(ctx), _creation_epoch(created_in), _resource(cc::move(resource)), _heap(cc::move(heap))
    {
    }

    // Deferred deletion: hands the GPU handle + finalizers to the context, freed once the owning epoch
    // retires (rather than freeing here, while the GPU may still be reading it). Body in .cc.
    ~dx12_texture() override;

    dx12_context& _ctx;                       // creating context — outlives this texture
    sg::epoch _creation_epoch;                // epoch this texture was created in (identity / diagnostics)
    mutable ComPtr<ID3D12Resource> _resource; // mutable: expiry releases it via a const hook
    sg::memory_heap_handle _heap;             // backing heap for a placed texture; null when dedicated

protected:
    // Release the GPU storage (deferred to epoch retire) when the texture is expired — see sg::raw_texture.
    void on_expired() const override;

private:
    // Shared by on_expired() and the destructor: stage the resource + finalizers for deferred deletion.
    // A no-op once already released (so expire()-then-destroy doesn't double-schedule).
    void release_storage() const;
};
} // namespace sg::backend::dx12
