#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/memory_heap.hh>

#include <memory>

namespace sg::backend::dx12
{
/// dx12 memory_heap: a GPU-resident ID3D12Heap (DEFAULT, buffers-only) that placed buffers
/// sub-allocate into via CreatePlacedResource. Reports per-buffer requirements from
/// GetResourceAllocationInfo. Keeps its own device ref so it can answer queries independently.
class dx12_memory_heap final : public sg::memory_heap
{
public:
    /// Creates a buffers-only DEFAULT heap. `size_in_bytes` must be >= 0; size 0 yields an empty heap
    /// (null ID3D12Heap) that holds no placements.
    [[nodiscard]] static cc::result<dx12_memory_heap_handle> create(ID3D12Device* device, cc::isize size_in_bytes);

    dx12_memory_heap(ComPtr<ID3D12Device> device, ComPtr<ID3D12Heap> heap, cc::isize size_in_bytes)
      : sg::memory_heap(size_in_bytes), _device(cc::move(device)), _heap(cc::move(heap))
    {
    }

    ComPtr<ID3D12Device> _device;
    ComPtr<ID3D12Heap> _heap; // null for a size-0 (empty) heap

protected:
    [[nodiscard]] sg::memory_requirements query_buffer_requirements(cc::isize size_in_bytes,
                                                                    sg::buffer_usage usage) const override;
};
} // namespace sg::backend::dx12
