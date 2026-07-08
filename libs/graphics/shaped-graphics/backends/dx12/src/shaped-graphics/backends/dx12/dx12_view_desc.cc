#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_format.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
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

// The D3D12 SRV desc for a texture view: dimension from the texture's shape, mip/array/plane from the
// view's subresource range. Multisampled SRVs (Load-based) and depth-as-SRV (typeless resource) are not
// supported yet.
[[nodiscard]] D3D12_SHADER_RESOURCE_VIEW_DESC texture_srv_desc(sg::texture_description const& d,
                                                               sg::subresource_range const& r,
                                                               DXGI_FORMAT format)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format = format;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    UINT const first_mip = UINT(r.mip_range.start);
    UINT const mips = UINT(r.mip_range.end - r.mip_range.start);
    UINT const first_slice = UINT(r.array_range.start);
    UINT const slices = UINT(r.array_range.end - r.array_range.start);
    UINT const plane = UINT(r.aspect_range.start);
    bool const is_array = d.array_layers.has_value();

    switch (d.dimension)
    {
    case sg::texture_dimension::d1:
        if (is_array)
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            desc.Texture1DArray = {first_mip, mips, first_slice, slices, 0.f};
        }
        else
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            desc.Texture1D = {first_mip, mips, 0.f};
        }
        break;
    case sg::texture_dimension::d2:
        CC_ASSERT(d.sample_count == 1, "multisampled texture SRV is not supported yet");
        if (d.is_cube)
        {
            if (is_array)
            {
                desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                desc.TextureCubeArray = {first_mip, mips, first_slice / 6, slices / 6, 0.f};
            }
            else
            {
                desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                desc.TextureCube = {first_mip, mips, 0.f};
            }
        }
        else if (is_array)
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {first_mip, mips, first_slice, slices, plane, 0.f};
        }
        else
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D = {first_mip, mips, plane, 0.f};
        }
        break;
    case sg::texture_dimension::d3:
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        desc.Texture3D = {first_mip, mips, 0.f};
        break;
    }
    return desc;
}

// The D3D12 UAV desc for a texture view (a single mip level; no MSAA, no cube — a cube is a 2D array).
[[nodiscard]] D3D12_UNORDERED_ACCESS_VIEW_DESC texture_uav_desc(sg::texture_description const& d,
                                                                sg::subresource_range const& r,
                                                                DXGI_FORMAT format)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format = format;

    UINT const mip = UINT(r.mip_range.start);
    UINT const first_slice = UINT(r.array_range.start);
    UINT const slices = UINT(r.array_range.end - r.array_range.start);
    UINT const plane = UINT(r.aspect_range.start);
    bool const is_array = d.array_layers.has_value() || d.is_cube;

    switch (d.dimension)
    {
    case sg::texture_dimension::d1:
        if (is_array)
        {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
            desc.Texture1DArray = {mip, first_slice, slices};
        }
        else
        {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            desc.Texture1D = {mip};
        }
        break;
    case sg::texture_dimension::d2:
        if (is_array)
        {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {mip, first_slice, slices, plane};
        }
        else
        {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            desc.Texture2D = {mip, plane};
        }
        break;
    case sg::texture_dimension::d3:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        desc.Texture3D = {mip, 0, UINT(d.depth)}; // all W slices
        break;
    }
    return desc;
}
} // namespace

void create_texture_view(ID3D12Device* device, sg::raw_view const& view, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    auto const* tex = dynamic_cast<dx12_texture const*>(view.texture.get());
    CC_ASSERT(tex != nullptr, "bound resource is not a dx12 texture");
    ID3D12Resource* const resource = tex->_resource.Get();
    DXGI_FORMAT const format = to_dxgi_format(view.format);

    switch (view.access)
    {
    case sg::view_class::readonly:
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC const desc = texture_srv_desc(tex->description(), view.range, format);
        device->CreateShaderResourceView(resource, &desc, dst);
        return;
    }
    case sg::view_class::readwrite:
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC const desc = texture_uav_desc(tex->description(), view.range, format);
        device->CreateUnorderedAccessView(resource, nullptr, &desc, dst); // no counter resource
        return;
    }
    default:
        CC_UNREACHABLE("unhandled texture view access class");
    }
}

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
