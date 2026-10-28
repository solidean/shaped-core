#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/memory/memory_heap.hh>


/// dx12 memory_heap: a GPU-resident ID3D12Heap (DEFAULT) that placed buffers or textures sub-allocate into via CreatePlacedResource.
/// Reports per-resource requirements from GetResourceAllocationInfo.
/// Keeps its own device ref, so it can answer queries independently.
class sg::backend::dx12::dx12_memory_heap final : public sg::memory_heap
{
public:
    /// Creates a DEFAULT heap holding `contents`.
    /// `size_in_bytes` must be >= 0; size 0 yields an empty heap (null ID3D12Heap) that holds no placements.
    /// A textures heap needs resource heap tier 2, and is refused below it.
    [[nodiscard]] static cc::result<dx12_memory_heap_handle> create(ID3D12Device* device,
                                                                    isize size_in_bytes,
                                                                    sg::memory_heap_contents contents);

    dx12_memory_heap(ComPtr<ID3D12Device> device,
                     ComPtr<ID3D12Heap> heap,
                     isize size_in_bytes,
                     sg::memory_heap_contents contents)
      : sg::memory_heap(size_in_bytes, contents), _device(cc::move(device)), _heap(cc::move(heap))
    {
    }

    ComPtr<ID3D12Device> _device;
    ComPtr<ID3D12Heap> _heap; // null for a size-0 (empty) heap

protected:
    [[nodiscard]] sg::memory_requirements query_buffer_requirements(isize size_in_bytes,
                                                                    sg::buffer_usages usage) const override;
    [[nodiscard]] sg::memory_requirements query_texture_requirements(sg::texture_description const& desc) const override;
};
