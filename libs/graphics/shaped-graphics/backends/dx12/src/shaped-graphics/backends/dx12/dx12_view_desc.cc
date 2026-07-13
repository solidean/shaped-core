#include <clean-core/common/assert.hh>
#include <shaped-graphics/attachment_views.hh>
#include <shaped-graphics/backends/dx12/dx12_acceleration_structure.hh>
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

// The D3D12 SRV desc for a texture view: dimension + mip/array/plane come straight from the view (a
// reinterpretation the view chose), not from the texture's shape. Non-array dimensions (Texture2D, cube,
// …) have no base-slice field in D3D12, so a non-zero first slice promotes to the size-1 array form —
// same texels, still declared as the requested dimension in the shader. depth-as-SRV (a typeless
// resource) is not supported yet.
[[nodiscard]] D3D12_SHADER_RESOURCE_VIEW_DESC texture_srv_desc(sg::raw_view const& v, DXGI_FORMAT format)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format = format;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    auto const& r = v.range;
    UINT const first_mip = UINT(r.mip_range.start);
    UINT const mips = UINT(r.mip_range.end - r.mip_range.start);
    UINT const first_slice = UINT(r.array_range.start);
    UINT const slices = UINT(r.array_range.end - r.array_range.start);
    UINT const plane = UINT(r.aspect_range.start);

    using vd = sg::texture_view_dimension;
    switch (v.view_dimension)
    {
    case vd::tex_1d:
        if (first_slice == 0)
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            desc.Texture1D = {first_mip, mips, 0.f};
        }
        else
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            desc.Texture1DArray = {first_mip, mips, first_slice, 1, 0.f};
        }
        break;
    case vd::tex_1d_array:
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        desc.Texture1DArray = {first_mip, mips, first_slice, slices, 0.f};
        break;
    case vd::tex_2d:
        if (first_slice == 0)
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D = {first_mip, mips, plane, 0.f};
        }
        else
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {first_mip, mips, first_slice, 1, plane, 0.f};
        }
        break;
    case vd::tex_2d_array:
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray = {first_mip, mips, first_slice, slices, plane, 0.f};
        break;
    case vd::tex_2d_ms:
        if (first_slice == 0)
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            desc.Texture2DMS = {};
        }
        else
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            desc.Texture2DMSArray = {first_slice, 1};
        }
        break;
    case vd::tex_2d_ms_array:
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
        desc.Texture2DMSArray = {first_slice, slices};
        break;
    case vd::tex_3d:
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        desc.Texture3D = {first_mip, mips, 0.f};
        break;
    case vd::cube:
        if (first_slice == 0)
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            desc.TextureCube = {first_mip, mips, 0.f};
        }
        else // no base-face field on TEXCUBE — a non-zero cube is a single-cube cube-array
        {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            desc.TextureCubeArray = {first_mip, mips, first_slice, 1, 0.f};
        }
        break;
    case vd::cube_array:
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        desc.TextureCubeArray = {first_mip, mips, first_slice, slices / 6, 0.f}; // First2DArrayFace, NumCubes
        break;
    }
    return desc;
}

// The D3D12 UAV desc for a texture view (a single mip level; no MSAA, no cube — a cube is a 2D array). A
// non-zero first slice on a non-array dimension promotes to the size-1 array form. 3D uses the view's
// W-slice window.
[[nodiscard]] D3D12_UNORDERED_ACCESS_VIEW_DESC texture_uav_desc(sg::raw_view const& v, DXGI_FORMAT format)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format = format;

    auto const& r = v.range;
    UINT const mip = UINT(r.mip_range.start);
    UINT const first_slice = UINT(r.array_range.start);
    UINT const slices = UINT(r.array_range.end - r.array_range.start);
    UINT const plane = UINT(r.aspect_range.start);

    using vd = sg::texture_view_dimension;
    switch (v.view_dimension)
    {
    case vd::tex_1d:
        if (first_slice == 0)
        {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            desc.Texture1D = {mip};
        }
        else
        {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
            desc.Texture1DArray = {mip, first_slice, 1};
        }
        break;
    case vd::tex_1d_array:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        desc.Texture1DArray = {mip, first_slice, slices};
        break;
    case vd::tex_2d:
        if (first_slice == 0)
        {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            desc.Texture2D = {mip, plane};
        }
        else
        {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {mip, first_slice, 1, plane};
        }
        break;
    case vd::tex_2d_array:
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray = {mip, first_slice, slices, plane};
        break;
    case vd::tex_3d:
    {
        UINT const first_w = UINT(v.depth_slice_range.start);
        UINT const w_count = UINT(v.depth_slice_range.end - v.depth_slice_range.start);
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        desc.Texture3D = {mip, first_w, w_count}; // FirstWSlice, WSize
        break;
    }
    case vd::tex_2d_ms:
    case vd::tex_2d_ms_array:
    case vd::cube:
    case vd::cube_array:
        CC_UNREACHABLE("multisampled / cube UAV is not valid");
    }
    return desc;
}

// The D3D12 RTV desc for a color attachment: a single mip level, 2D-shaped (a cube renders as a 2D
// array), MSAA allowed. A non-zero first slice on a non-array dimension promotes to the size-1 array form.
[[nodiscard]] D3D12_RENDER_TARGET_VIEW_DESC texture_rtv_desc(sg::render_target_view const& v, DXGI_FORMAT format)
{
    D3D12_RENDER_TARGET_VIEW_DESC desc = {};
    desc.Format = format;

    auto const& r = v.range();
    UINT const mip = UINT(r.mip_range.start);
    UINT const first_slice = UINT(r.array_range.start);
    UINT const slices = UINT(r.array_range.end - r.array_range.start);
    UINT const plane = UINT(r.aspect_range.start);

    using vd = sg::texture_view_dimension;
    switch (v.dimension())
    {
    case vd::tex_2d:
        if (first_slice == 0)
        {
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            desc.Texture2D = {mip, plane};
        }
        else
        {
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {mip, first_slice, 1, plane};
        }
        break;
    case vd::tex_2d_array:
        desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray = {mip, first_slice, slices, plane};
        break;
    case vd::tex_2d_ms:
        if (first_slice == 0)
        {
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
            desc.Texture2DMS = {};
        }
        else
        {
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            desc.Texture2DMSArray = {first_slice, 1};
        }
        break;
    case vd::tex_2d_ms_array:
        desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
        desc.Texture2DMSArray = {first_slice, slices};
        break;
    default:
        CC_UNREACHABLE("render-target views are 2D-shaped only");
    }
    return desc;
}

// The D3D12 DSV desc for a depth/stencil attachment: like the RTV, but there is no PlaneSlice (a DSV
// covers the depth (+stencil) planes implicitly) and it carries the read-only flags (none here).
[[nodiscard]] D3D12_DEPTH_STENCIL_VIEW_DESC texture_dsv_desc(sg::depth_stencil_view const& v, DXGI_FORMAT format)
{
    D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
    desc.Format = format;
    desc.Flags = D3D12_DSV_FLAG_NONE;

    auto const& r = v.range();
    UINT const mip = UINT(r.mip_range.start);
    UINT const first_slice = UINT(r.array_range.start);
    UINT const slices = UINT(r.array_range.end - r.array_range.start);

    using vd = sg::texture_view_dimension;
    switch (v.dimension())
    {
    case vd::tex_2d:
        if (first_slice == 0)
        {
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            desc.Texture2D = {mip};
        }
        else
        {
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {mip, first_slice, 1};
        }
        break;
    case vd::tex_2d_array:
        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray = {mip, first_slice, slices};
        break;
    case vd::tex_2d_ms:
        if (first_slice == 0)
        {
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
            desc.Texture2DMS = {};
        }
        else
        {
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            desc.Texture2DMSArray = {first_slice, 1};
        }
        break;
    case vd::tex_2d_ms_array:
        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
        desc.Texture2DMSArray = {first_slice, slices};
        break;
    default:
        CC_UNREACHABLE("depth-stencil views are 2D-shaped only");
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
        D3D12_SHADER_RESOURCE_VIEW_DESC const desc = texture_srv_desc(view, format);
        device->CreateShaderResourceView(resource, &desc, dst);
        return;
    }
    case sg::view_class::readwrite:
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC const desc = texture_uav_desc(view, format);
        device->CreateUnorderedAccessView(resource, nullptr, &desc, dst); // no counter resource
        return;
    }
    default:
        CC_UNREACHABLE("unhandled texture view access class");
    }
}

void create_render_target_view(ID3D12Device* device, sg::render_target_view const& view, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    auto const* tex = dynamic_cast<dx12_texture const*>(view.texture().get());
    CC_ASSERT(tex != nullptr, "bound resource is not a dx12 texture");
    D3D12_RENDER_TARGET_VIEW_DESC const desc = texture_rtv_desc(view, to_dxgi_format(view.format()));
    device->CreateRenderTargetView(tex->_resource.Get(), &desc, dst);
}

void create_depth_stencil_view(ID3D12Device* device, sg::depth_stencil_view const& view, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    auto const* tex = dynamic_cast<dx12_texture const*>(view.texture().get());
    CC_ASSERT(tex != nullptr, "bound resource is not a dx12 texture");
    D3D12_DEPTH_STENCIL_VIEW_DESC const desc = texture_dsv_desc(view, to_dxgi_format(view.format()));
    device->CreateDepthStencilView(tex->_resource.Get(), &desc, dst);
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
    case sg::view_class::acceleration_structure:
        CC_UNREACHABLE("acceleration-structure views are created via create_accel_view, not create_buffer_view");
    }
    CC_UNREACHABLE("unhandled view access class");
}

void create_accel_view(ID3D12Device* device, dx12_tlas const& tlas, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    // A ray-tracing AS SRV is special: it is addressed purely by the AS's GPU virtual address, so the resource
    // argument to CreateShaderResourceView is null and the location lives in the desc.
    CC_ASSERT(tlas._dx12_storage != nullptr, "acceleration structure has no storage buffer");
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.RaytracingAccelerationStructure.Location = tlas._dx12_storage->gpu_virtual_address();
    device->CreateShaderResourceView(nullptr, &desc, dst);
}
} // namespace sg::backend::dx12
