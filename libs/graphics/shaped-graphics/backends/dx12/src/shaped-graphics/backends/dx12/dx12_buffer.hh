#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/fwd.hh>

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

protected:
    // Release the GPU storage (deferred to epoch retire) when the buffer is expired — see sg::buffer.
    void on_expired() const override;

private:
    // Shared by on_expired() and the destructor: stage the resource + finalizers for deferred deletion.
    // A no-op once already released (so expire()-then-destroy doesn't double-schedule).
    void release_storage() const;
};
} // namespace sg::backend::dx12
