// dx12_buffer: GPU buffer creation and deferred-deletion destructor. The buffer type is otherwise
// header-only (ctor + fields).

#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
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

cc::result<dx12_buffer_handle> dx12_context::create_dx12_buffer(cc::isize size_in_bytes, sg::buffer_usage usage)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");

    ComPtr<ID3D12Resource> resource;

    // Empty buffer: no allocation (D3D12 rejects a zero-width resource); null is the representation.
    if (size_in_bytes > 0)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT; // GPU-resident: sg exposes no host-visible buffers.

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = UINT64(size_in_bytes);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; // required for buffers
        desc.Flags = sg::has_flag(usage, sg::buffer_usage::storage) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                                    : D3D12_RESOURCE_FLAG_NONE;

        // Created in COMMON: buffer copies rely on D3D12 implicit state promotion/decay (a buffer is
        // promoted from COMMON to COPY_DEST / COPY_SOURCE on use and decays back at ExecuteCommandLists),
        // so no explicit barriers are recorded for transfers.
        // TODO: a real per-resource barrier + state-tracking system will replace this (and enable, e.g.,
        // uploading then downloading the same buffer within one command list).
        if (HRESULT hr = _device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
            FAILED(hr))
            return dx12_error(hr, "ID3D12Device::CreateCommittedResource failed");
    }

    return std::make_shared<dx12_buffer>(*this, current_epoch(), size_in_bytes, usage, cc::move(resource));
}
} // namespace sg::backend::dx12
