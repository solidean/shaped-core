#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_view_desc.hh>
#include <shaped-graphics/views.hh>

namespace sg::backend::dx12
{
namespace
{
// The underlying ID3D12Resource of a view's buffer (null for an empty buffer). Raw-word views address
// in units of 4 bytes; structured views in units of the element stride.
[[nodiscard]] ID3D12Resource* resource_of(sg::raw_view const& view)
{
    auto const* buf = dynamic_cast<dx12_buffer const*>(view.buffer.get());
    CC_ASSERT(buf != nullptr, "bound resource is not a dx12 buffer");
    return buf->_resource.Get();
}
} // namespace

void create_buffer_view(ID3D12Device* device, sg::raw_view const& view, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    ID3D12Resource* const resource = resource_of(view);

    switch (view.access)
    {
    case sg::view_class::uniform:
    {
        CC_ASSERT(resource != nullptr, "uniform buffer view over an empty buffer");
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
        desc.BufferLocation = resource->GetGPUVirtualAddress() + UINT64(view.offset_in_bytes);
        desc.SizeInBytes = UINT((view.size_in_bytes + 255) & ~cc::isize(255)); // CBV size is 256-aligned
        device->CreateConstantBufferView(&desc, dst);
        return;
    }
    case sg::view_class::readonly:
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        if (view.shape == sg::view_shape::raw)
        {
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.Buffer.FirstElement = UINT64(view.offset_in_bytes / 4);
            desc.Buffer.NumElements = UINT(view.size_in_bytes / 4);
            desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        }
        else // structured
        {
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.Buffer.FirstElement
                = UINT64(view.stride_in_bytes != 0 ? view.offset_in_bytes / view.stride_in_bytes : 0);
            desc.Buffer.NumElements = UINT(view.element_count);
            desc.Buffer.StructureByteStride = UINT(view.stride_in_bytes);
        }
        device->CreateShaderResourceView(resource, &desc, dst);
        return;
    }
    case sg::view_class::readwrite:
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        if (view.shape == sg::view_shape::raw)
        {
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.Buffer.FirstElement = UINT64(view.offset_in_bytes / 4);
            desc.Buffer.NumElements = UINT(view.size_in_bytes / 4);
            desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        }
        else // structured
        {
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.Buffer.FirstElement
                = UINT64(view.stride_in_bytes != 0 ? view.offset_in_bytes / view.stride_in_bytes : 0);
            desc.Buffer.NumElements = UINT(view.element_count);
            desc.Buffer.StructureByteStride = UINT(view.stride_in_bytes);
        }
        device->CreateUnorderedAccessView(resource, nullptr, &desc, dst); // no counter resource
        return;
    }
    }
    CC_UNREACHABLE("unhandled view access class");
}
} // namespace sg::backend::dx12
