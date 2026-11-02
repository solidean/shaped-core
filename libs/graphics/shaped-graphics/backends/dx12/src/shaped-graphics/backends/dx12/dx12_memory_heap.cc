// dx12_memory_heap: DEFAULT heap creation and per-resource requirement queries, plus the context entry point create_dx12_memory_heap.
// Placement itself happens in create_dx12_buffer and create_dx12_texture.

#include <clean-core/common/assert.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_memory_heap.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>

namespace sg::backend::dx12
{
cc::result<dx12_memory_heap_handle> dx12_memory_heap::create(ID3D12Device* device,
                                                             isize size_in_bytes,
                                                             sg::memory_heap_contents contents)
{
    CC_ASSERT(size_in_bytes >= 0, "memory heap size must be non-negative");

    auto const textures = contents == sg::memory_heap_contents::textures;
    if (textures)
    {
        // Tier 1 splits textures by whether they are render targets, and the transient textures sg places are both.
        auto options = D3D12_FEATURE_DATA_D3D12_OPTIONS{};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))
            || options.ResourceHeapTier < D3D12_RESOURCE_HEAP_TIER_2)
            return cc::error("a textures heap needs D3D12 resource heap tier 2");
    }

    ComPtr<ID3D12Heap> heap;
    // D3D12 rejects a zero-size heap; represent an empty heap as a null ID3D12Heap (no placements fit).
    if (size_in_bytes > 0)
    {
        D3D12_HEAP_DESC desc = {};
        desc.SizeInBytes = UINT64(size_in_bytes);
        desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT; // GPU-resident, matching committed buffers
        desc.Alignment = 0;                             // 0 => 64 KiB default resource placement alignment
        desc.Flags = textures ? D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES : D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        if (HRESULT hr = device->CreateHeap(&desc, IID_PPV_ARGS(&heap)); FAILED(hr))
            return dx12_error(hr, "ID3D12Device::CreateHeap failed");
    }

    ComPtr<ID3D12Device> dev = device; // hold a ref so requirement queries work independent of the context
    return dx12_memory_heap_handle(
        std::make_shared<dx12_memory_heap>(cc::move(dev), cc::move(heap), size_in_bytes, contents));
}

sg::memory_requirements dx12_memory_heap::query_buffer_requirements(isize size_in_bytes, sg::buffer_usages usage) const
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");
    // An empty buffer occupies nothing; report the default placement alignment so any valid offset works.
    if (size_in_bytes == 0)
        return {.alignment_in_bytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, .size_in_bytes = 0};

    D3D12_RESOURCE_DESC const desc = buffer_resource_desc(size_in_bytes, usage);
    D3D12_RESOURCE_ALLOCATION_INFO const info = _device->GetResourceAllocationInfo(0, 1, &desc);
    return {.alignment_in_bytes = isize(info.Alignment), .size_in_bytes = isize(info.SizeInBytes)};
}

sg::memory_requirements dx12_memory_heap::query_texture_requirements(sg::texture_description const& desc) const
{
    // A multisampled texture needs the 4 MiB placement alignment, which a heap of the default alignment cannot give.
    if (desc.sample_count > 1)
        return {.placeable = false};

    D3D12_RESOURCE_DESC const rdesc = texture_resource_desc(desc);
    D3D12_RESOURCE_ALLOCATION_INFO const info = _device->GetResourceAllocationInfo(0, 1, &rdesc);
    // UINT64_MAX is the device rejecting the description outright.
    if (info.SizeInBytes == UINT64_MAX)
        return {.placeable = false};
    return {.alignment_in_bytes = isize(info.Alignment), .size_in_bytes = isize(info.SizeInBytes)};
}

cc::result<dx12_memory_heap_handle> dx12_context::create_dx12_memory_heap(isize size_in_bytes,
                                                                          sg::memory_heap_contents contents)
{
    CC_RECORD_ACCUM("sg.heap.bytes_allocated", cc::rec::unit_bytes, size_in_bytes);

    return dx12_memory_heap::create(_device.Get(), size_in_bytes, contents);
}
} // namespace sg::backend::dx12
