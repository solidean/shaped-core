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
                sg::memory_heap_handle heap = nullptr,
                bool is_transient = false)
      : sg::buffer(size_in_bytes, usage),
        _ctx(ctx),
        _creation_epoch(created_in),
        _resource(cc::move(resource)),
        _heap(cc::move(heap)),
        _is_transient(is_transient)
    {
    }

    // Deferred deletion: hands the GPU handle + finalizers to the context, freed once the owning
    // epoch retires (rather than freeing here, while the GPU may still be reading it). Body in .cc.
    ~dx12_buffer() override;

    /// True once a transient buffer's epoch has passed: its storage may have been recycled, so using it
    /// (in a transfer or a binding) is a hard error. Always false for a persistent buffer. Body in .cc
    /// (reads the context's current epoch).
    [[nodiscard]] bool is_expired() const;

    dx12_context& _ctx;        // creating context — outlives this buffer
    sg::epoch _creation_epoch; // epoch this buffer was created in (transient expiry / identity / diagnostics)
    ComPtr<ID3D12Resource> _resource;
    sg::memory_heap_handle _heap; // backing heap for a placed buffer; null when dedicated (committed)
    bool _is_transient;           // transient buffers expire when _creation_epoch passes
};
} // namespace sg::backend::dx12
