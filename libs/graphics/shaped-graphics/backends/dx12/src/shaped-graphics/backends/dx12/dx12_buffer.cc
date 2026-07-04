// dx12_buffer: GPU buffer creation (committed + placed) and deferred-deletion destructor. The buffer
// type is otherwise header-only (ctor + fields).

#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_memory_heap.hh>

namespace sg::backend::dx12
{
D3D12_RESOURCE_DESC buffer_resource_desc(cc::isize size_in_bytes, sg::buffer_usage usage)
{
    CC_ASSERT(size_in_bytes > 0, "buffer resource desc requires a positive size");

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = UINT64(size_in_bytes);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; // required for buffers
    // Only a UAV (read-write storage) needs a creation flag; SRV / CBV / VBV / IBV / copy / indirect
    // are all allowed by default on a D3D12 buffer.
    desc.Flags = sg::has_flag(usage, sg::buffer_usage::readwrite_buffer) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                                         : D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

dx12_buffer::~dx12_buffer()
{
    // Stage the GPU handle + finalizers for deletion once the current epoch retires. Empty buffers
    // (null resource) with no finalizers own nothing GPU-side and need no deferral.
    if (_resource || !_finalizers.empty())
    {
        dx12_expiring_resource expiring;
        expiring.resource = cc::move(_resource);
        expiring.finalizers = cc::move(_finalizers);
        _ctx.schedule_deferred_deletion(cc::move(expiring));
    }
}

cc::result<dx12_buffer_handle> dx12_context::create_dx12_buffer(cc::isize size_in_bytes,
                                                                sg::buffer_usage usage,
                                                                sg::allocation_info const& alloc)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");

    ComPtr<ID3D12Resource> resource;
    sg::memory_heap_handle heap_handle; // held by the buffer for a placed resource; null when dedicated

    // Empty buffer: no allocation (D3D12 rejects a zero-width resource); null is the representation.
    if (size_in_bytes > 0)
    {
        D3D12_RESOURCE_DESC const desc = buffer_resource_desc(size_in_bytes, usage);

        // Created in COMMON: buffer copies rely on D3D12 implicit state promotion/decay (a buffer is
        // promoted from COMMON to COPY_DEST / COPY_SOURCE on use and decays back at ExecuteCommandLists),
        // so no explicit barriers are recorded for transfers.
        // TODO: a real per-resource barrier + state-tracking system will replace this (and enable, e.g.,
        // uploading then downloading the same buffer within one command list).
        if (alloc.is_placed())
        {
            // Placed: sub-allocate into the caller's heap at the offset its allocator picked.
            auto const dx_heap = std::dynamic_pointer_cast<dx12_memory_heap const>(alloc.heap);
            CC_ASSERT(dx_heap != nullptr, "memory_heap is not a dx12 memory_heap");
            CC_ASSERT(dx_heap->_heap != nullptr, "cannot place a buffer into an empty (size 0) heap");
            CC_ASSERT(alloc.offset >= 0, "placement offset must be non-negative");
            CC_ASSERT(alloc.offset + size_in_bytes <= dx_heap->size_in_bytes(), "placement exceeds the heap");
            if (HRESULT hr = _device->CreatePlacedResource(dx_heap->_heap.Get(), UINT64(alloc.offset), &desc,
                                                           D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
                FAILED(hr))
                return dx12_error(hr, "ID3D12Device::CreatePlacedResource failed");
            heap_handle = alloc.heap;
        }
        else
        {
            // Dedicated: a committed resource owns its own allocation.
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT; // GPU-resident: sg exposes no host-visible buffers.
            if (HRESULT hr = _device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
                FAILED(hr))
                return dx12_error(hr, "ID3D12Device::CreateCommittedResource failed");
        }
    }

    return std::make_shared<dx12_buffer>(*this, current_epoch(), size_in_bytes, usage, cc::move(resource),
                                         cc::move(heap_handle));
}
} // namespace sg::backend::dx12
