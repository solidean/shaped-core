// dx12_buffer: GPU buffer creation. The buffer type itself is header-only (ctor + fields); the
// allocating create path lives here.

#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
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

        D3D12_RESOURCE_STATES initial_state = sg::has_flag(usage, sg::buffer_usage::copy_dst)
                                                ? D3D12_RESOURCE_STATE_COPY_DEST
                                                : D3D12_RESOURCE_STATE_COMMON;

        if (HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state, nullptr,
                                                          IID_PPV_ARGS(&resource));
            FAILED(hr))
            return dx12_error(hr, "ID3D12Device::CreateCommittedResource failed");
    }

    return std::make_shared<dx12_buffer>(*this, size_in_bytes, usage, cc::move(resource));
}
} // namespace sg::backend::dx12
