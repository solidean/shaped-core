// dx12_memory_heap: DEFAULT heap creation and per-buffer requirement queries, plus the context
// entry point create_dx12_memory_heap. Placement itself happens in create_dx12_buffer.

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_memory_heap.hh>

namespace sg::backend::dx12
{
cc::result<dx12_memory_heap_handle> dx12_memory_heap::create(ID3D12Device* device, cc::isize size_in_bytes)
{
    CC_ASSERT(size_in_bytes >= 0, "memory heap size must be non-negative");

    ComPtr<ID3D12Heap> heap;
    // D3D12 rejects a zero-size heap; represent an empty heap as a null ID3D12Heap (no placements fit).
    if (size_in_bytes > 0)
    {
        D3D12_HEAP_DESC desc = {};
        desc.SizeInBytes = UINT64(size_in_bytes);
        desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;  // GPU-resident, matching committed buffers
        desc.Alignment = 0;                              // 0 => 64 KiB default resource placement alignment
        desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS; // sg exposes only buffers today
        if (HRESULT hr = device->CreateHeap(&desc, IID_PPV_ARGS(&heap)); FAILED(hr))
            return dx12_error(hr, "ID3D12Device::CreateHeap failed");
    }

    ComPtr<ID3D12Device> dev = device; // hold a ref so requirement queries work independent of the context
    return dx12_memory_heap_handle(std::make_shared<dx12_memory_heap>(cc::move(dev), cc::move(heap), size_in_bytes));
}

sg::memory_requirements dx12_memory_heap::query_buffer_requirements(cc::isize size_in_bytes, sg::buffer_usage usage) const
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");
    // An empty buffer occupies nothing; report the default placement alignment so any valid offset works.
    if (size_in_bytes == 0)
        return {.alignment_in_bytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, .size_in_bytes = 0};

    D3D12_RESOURCE_DESC const desc = buffer_resource_desc(size_in_bytes, usage);
    D3D12_RESOURCE_ALLOCATION_INFO const info = _device->GetResourceAllocationInfo(0, 1, &desc);
    return {.alignment_in_bytes = cc::isize(info.Alignment), .size_in_bytes = cc::isize(info.SizeInBytes)};
}

cc::result<dx12_memory_heap_handle> dx12_context::create_dx12_memory_heap(cc::isize size_in_bytes)
{
    return dx12_memory_heap::create(_device.Get(), size_in_bytes);
}
} // namespace sg::backend::dx12
