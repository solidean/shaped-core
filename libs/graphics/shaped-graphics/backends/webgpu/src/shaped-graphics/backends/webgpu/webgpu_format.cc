#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/webgpu/webgpu_format.hh>

namespace sg::backend::webgpu
{
WGPUTextureFormat to_wgpu_format(sg::pixel_format f)
{
    switch (f)
    {
    case sg::pixel_format::undefined:
        return WGPUTextureFormat_Undefined;
    case sg::pixel_format::r8_unorm:
        return WGPUTextureFormat_R8Unorm;
    case sg::pixel_format::r8_snorm:
        return WGPUTextureFormat_R8Snorm;
    case sg::pixel_format::r8_uint:
        return WGPUTextureFormat_R8Uint;
    case sg::pixel_format::r8_sint:
        return WGPUTextureFormat_R8Sint;
    case sg::pixel_format::rg8_unorm:
        return WGPUTextureFormat_RG8Unorm;
    case sg::pixel_format::rg8_snorm:
        return WGPUTextureFormat_RG8Snorm;
    case sg::pixel_format::rg8_uint:
        return WGPUTextureFormat_RG8Uint;
    case sg::pixel_format::rg8_sint:
        return WGPUTextureFormat_RG8Sint;
    case sg::pixel_format::rgba8_unorm:
        return WGPUTextureFormat_RGBA8Unorm;
    case sg::pixel_format::rgba8_snorm:
        return WGPUTextureFormat_RGBA8Snorm;
    case sg::pixel_format::rgba8_uint:
        return WGPUTextureFormat_RGBA8Uint;
    case sg::pixel_format::rgba8_sint:
        return WGPUTextureFormat_RGBA8Sint;
    case sg::pixel_format::rgba8_unorm_srgb:
        return WGPUTextureFormat_RGBA8UnormSrgb;
    case sg::pixel_format::bgra8_unorm:
        return WGPUTextureFormat_BGRA8Unorm;
    case sg::pixel_format::bgra8_unorm_srgb:
        return WGPUTextureFormat_BGRA8UnormSrgb;
    case sg::pixel_format::r16_float:
        return WGPUTextureFormat_R16Float;
    case sg::pixel_format::r16_uint:
        return WGPUTextureFormat_R16Uint;
    case sg::pixel_format::r16_sint:
        return WGPUTextureFormat_R16Sint;
    case sg::pixel_format::rg16_float:
        return WGPUTextureFormat_RG16Float;
    case sg::pixel_format::rg16_uint:
        return WGPUTextureFormat_RG16Uint;
    case sg::pixel_format::rg16_sint:
        return WGPUTextureFormat_RG16Sint;
    case sg::pixel_format::rgba16_float:
        return WGPUTextureFormat_RGBA16Float;
    case sg::pixel_format::rgba16_uint:
        return WGPUTextureFormat_RGBA16Uint;
    case sg::pixel_format::rgba16_sint:
        return WGPUTextureFormat_RGBA16Sint;
    case sg::pixel_format::r32_float:
        return WGPUTextureFormat_R32Float;
    case sg::pixel_format::r32_uint:
        return WGPUTextureFormat_R32Uint;
    case sg::pixel_format::r32_sint:
        return WGPUTextureFormat_R32Sint;
    case sg::pixel_format::rg32_float:
        return WGPUTextureFormat_RG32Float;
    case sg::pixel_format::rg32_uint:
        return WGPUTextureFormat_RG32Uint;
    case sg::pixel_format::rg32_sint:
        return WGPUTextureFormat_RG32Sint;
    case sg::pixel_format::rgba32_float:
        return WGPUTextureFormat_RGBA32Float;
    case sg::pixel_format::rgba32_uint:
        return WGPUTextureFormat_RGBA32Uint;
    case sg::pixel_format::rgba32_sint:
        return WGPUTextureFormat_RGBA32Sint;
    case sg::pixel_format::rgb10a2_unorm:
        return WGPUTextureFormat_RGB10A2Unorm;
    case sg::pixel_format::rg11b10_float:
        return WGPUTextureFormat_RG11B10Ufloat;
    case sg::pixel_format::depth16_unorm:
        return WGPUTextureFormat_Depth16Unorm;
    case sg::pixel_format::depth32_float:
        return WGPUTextureFormat_Depth32Float;
    case sg::pixel_format::depth32_float_stencil8:
        return WGPUTextureFormat_Depth32FloatStencil8;
    case sg::pixel_format::bc1_rgba_unorm:
        return WGPUTextureFormat_BC1RGBAUnorm;
    case sg::pixel_format::bc1_rgba_unorm_srgb:
        return WGPUTextureFormat_BC1RGBAUnormSrgb;
    case sg::pixel_format::bc2_unorm:
        return WGPUTextureFormat_BC2RGBAUnorm;
    case sg::pixel_format::bc2_unorm_srgb:
        return WGPUTextureFormat_BC2RGBAUnormSrgb;
    case sg::pixel_format::bc3_unorm:
        return WGPUTextureFormat_BC3RGBAUnorm;
    case sg::pixel_format::bc3_unorm_srgb:
        return WGPUTextureFormat_BC3RGBAUnormSrgb;
    case sg::pixel_format::bc4_r_unorm:
        return WGPUTextureFormat_BC4RUnorm;
    case sg::pixel_format::bc4_r_snorm:
        return WGPUTextureFormat_BC4RSnorm;
    case sg::pixel_format::bc5_rg_unorm:
        return WGPUTextureFormat_BC5RGUnorm;
    case sg::pixel_format::bc5_rg_snorm:
        return WGPUTextureFormat_BC5RGSnorm;
    case sg::pixel_format::bc6h_rgb_ufloat:
        return WGPUTextureFormat_BC6HRGBUfloat;
    case sg::pixel_format::bc6h_rgb_sfloat:
        return WGPUTextureFormat_BC6HRGBFloat;
    case sg::pixel_format::bc7_rgba_unorm:
        return WGPUTextureFormat_BC7RGBAUnorm;
    case sg::pixel_format::bc7_rgba_unorm_srgb:
        return WGPUTextureFormat_BC7RGBAUnormSrgb;
    }
    CC_UNREACHABLE("unhandled pixel_format in to_wgpu_format");
}

WGPUBufferUsage to_wgpu_buffer_usage(sg::buffer_usages usage)
{
    auto out = WGPUBufferUsage_None;
    if (usage.has(sg::buffer_usage::copy_src))
        out |= WGPUBufferUsage_CopySrc;
    if (usage.has(sg::buffer_usage::copy_dst))
        out |= WGPUBufferUsage_CopyDst;
    if (usage.has(sg::buffer_usage::vertex_buffer))
        out |= WGPUBufferUsage_Vertex;
    if (usage.has(sg::buffer_usage::index_buffer))
        out |= WGPUBufferUsage_Index;
    if (usage.has(sg::buffer_usage::constants_buffer))
        out |= WGPUBufferUsage_Uniform;
    if (usage.has_any(sg::buffer_usage::readonly_buffer | sg::buffer_usage::readwrite_buffer))
        out |= WGPUBufferUsage_Storage;
    if (usage.has(sg::buffer_usage::indirect_command_buffer))
        out |= WGPUBufferUsage_Indirect;
    return out;
}

WGPUTextureUsage to_wgpu_texture_usage(sg::texture_usages usage)
{
    auto out = WGPUTextureUsage_None;
    if (usage.has(sg::texture_usage::copy_src))
        out |= WGPUTextureUsage_CopySrc;
    if (usage.has(sg::texture_usage::copy_dst))
        out |= WGPUTextureUsage_CopyDst;
    if (usage.has(sg::texture_usage::texture))
        out |= WGPUTextureUsage_TextureBinding;
    if (usage.has(sg::texture_usage::image))
        out |= WGPUTextureUsage_StorageBinding;
    if (usage.has_any(sg::texture_usage::render_target | sg::texture_usage::depth_stencil))
        out |= WGPUTextureUsage_RenderAttachment;
    return out;
}

WGPUTextureDimension to_wgpu_texture_dimension(sg::texture_dimension d)
{
    return d == sg::texture_dimension::d3 ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
}

WGPUTextureViewDimension to_wgpu_view_dimension(sg::texture_view_dimension d)
{
    switch (d)
    {
    case sg::texture_view_dimension::tex_1d:
    case sg::texture_view_dimension::tex_2d:
    case sg::texture_view_dimension::tex_2d_ms:
        return WGPUTextureViewDimension_2D;
    case sg::texture_view_dimension::tex_1d_array:
    case sg::texture_view_dimension::tex_2d_array:
    case sg::texture_view_dimension::tex_2d_ms_array:
        return WGPUTextureViewDimension_2DArray;
    case sg::texture_view_dimension::tex_3d:
        return WGPUTextureViewDimension_3D;
    case sg::texture_view_dimension::cube:
        return WGPUTextureViewDimension_Cube;
    case sg::texture_view_dimension::cube_array:
        return WGPUTextureViewDimension_CubeArray;
    }
    CC_UNREACHABLE("unhandled texture_view_dimension in to_wgpu_view_dimension");
}

WGPUTextureAspect to_wgpu_aspect(sg::pixel_format format, cc::start_end aspect_range)
{
    if (!sg::is_depth_format(format) || aspect_range.end - aspect_range.start >= sg::format_aspect_count(format))
        return WGPUTextureAspect_All;
    return sg::format_aspect_at(format, aspect_range.start) == sg::texture_aspect::stencil
             ? WGPUTextureAspect_StencilOnly
             : WGPUTextureAspect_DepthOnly;
}

WGPUTextureAspect to_wgpu_copy_aspect(sg::pixel_format format, sg::texture_aspect aspect)
{
    if (!sg::has_stencil(format))
        return WGPUTextureAspect_All;
    return aspect == sg::texture_aspect::stencil ? WGPUTextureAspect_StencilOnly : WGPUTextureAspect_DepthOnly;
}

WGPUShaderStage to_wgpu_visibility(sg::shader_stages visibility, bool writable)
{
    if (visibility.is_empty())
    {
        return writable ? WGPUShaderStage_Fragment | WGPUShaderStage_Compute
                        : WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
    }

    auto out = WGPUShaderStage_None;
    if (visibility.has(sg::shader_stage::vertex))
        out |= WGPUShaderStage_Vertex;
    if (visibility.has(sg::shader_stage::fragment))
        out |= WGPUShaderStage_Fragment;
    if (visibility.has(sg::shader_stage::compute))
        out |= WGPUShaderStage_Compute;
    return out;
}

WGPUTextureSampleType default_sample_type(sg::pixel_format format)
{
    switch (format)
    {
    case sg::pixel_format::r8_uint:
    case sg::pixel_format::rg8_uint:
    case sg::pixel_format::rgba8_uint:
    case sg::pixel_format::r16_uint:
    case sg::pixel_format::rg16_uint:
    case sg::pixel_format::rgba16_uint:
    case sg::pixel_format::r32_uint:
    case sg::pixel_format::rg32_uint:
    case sg::pixel_format::rgba32_uint:
        return WGPUTextureSampleType_Uint;
    case sg::pixel_format::r8_sint:
    case sg::pixel_format::rg8_sint:
    case sg::pixel_format::rgba8_sint:
    case sg::pixel_format::r16_sint:
    case sg::pixel_format::rg16_sint:
    case sg::pixel_format::rgba16_sint:
    case sg::pixel_format::r32_sint:
    case sg::pixel_format::rg32_sint:
    case sg::pixel_format::rgba32_sint:
        return WGPUTextureSampleType_Sint;
    case sg::pixel_format::r32_float:
    case sg::pixel_format::rg32_float:
    case sg::pixel_format::rgba32_float:
        return WGPUTextureSampleType_UnfilterableFloat;
    case sg::pixel_format::depth16_unorm:
    case sg::pixel_format::depth32_float:
    case sg::pixel_format::depth32_float_stencil8:
        return WGPUTextureSampleType_Depth;
    default:
        return WGPUTextureSampleType_Float;
    }
}

WGPUTextureSampleType to_wgpu_sample_type(sg::texture_sample_type t)
{
    switch (t)
    {
    case sg::texture_sample_type::filterable_float:
        return WGPUTextureSampleType_Float;
    case sg::texture_sample_type::unfilterable_float:
        return WGPUTextureSampleType_UnfilterableFloat;
    case sg::texture_sample_type::depth:
        return WGPUTextureSampleType_Depth;
    case sg::texture_sample_type::sint:
        return WGPUTextureSampleType_Sint;
    case sg::texture_sample_type::uint:
        return WGPUTextureSampleType_Uint;
    }
    CC_UNREACHABLE("unhandled texture_sample_type");
}

WGPUSamplerBindingType to_wgpu_sampler_binding_type(sg::sampler_binding_type t)
{
    switch (t)
    {
    case sg::sampler_binding_type::filtering:
        return WGPUSamplerBindingType_Filtering;
    case sg::sampler_binding_type::non_filtering:
        return WGPUSamplerBindingType_NonFiltering;
    case sg::sampler_binding_type::comparison:
        return WGPUSamplerBindingType_Comparison;
    }
    CC_UNREACHABLE("unhandled sampler_binding_type");
}

WGPUCompareFunction to_wgpu_compare(sg::compare_op op)
{
    switch (op)
    {
    case sg::compare_op::never:
        return WGPUCompareFunction_Never;
    case sg::compare_op::less:
        return WGPUCompareFunction_Less;
    case sg::compare_op::equal:
        return WGPUCompareFunction_Equal;
    case sg::compare_op::less_equal:
        return WGPUCompareFunction_LessEqual;
    case sg::compare_op::greater:
        return WGPUCompareFunction_Greater;
    case sg::compare_op::not_equal:
        return WGPUCompareFunction_NotEqual;
    case sg::compare_op::greater_equal:
        return WGPUCompareFunction_GreaterEqual;
    case sg::compare_op::always:
        return WGPUCompareFunction_Always;
    }
    CC_UNREACHABLE("unhandled compare_op");
}

namespace
{
WGPUAddressMode to_wgpu_address(sg::sampler_address_mode m)
{
    switch (m)
    {
    case sg::sampler_address_mode::repeat:
        return WGPUAddressMode_Repeat;
    case sg::sampler_address_mode::mirror_repeat:
        return WGPUAddressMode_MirrorRepeat;
    case sg::sampler_address_mode::clamp_edge:
        return WGPUAddressMode_ClampToEdge;
    }
    CC_UNREACHABLE("unhandled sampler_address_mode");
}

WGPUFilterMode to_wgpu_filter(sg::sampler_filter f)
{
    return f == sg::sampler_filter::linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
}
} // namespace

WGPUSamplerDescriptor to_wgpu_sampler(sg::sampler const& s)
{
    auto const all_linear = s.min_filter == sg::sampler_filter::linear && s.mag_filter == sg::sampler_filter::linear
                         && s.mip_filter == sg::sampler_filter::linear;

    // WebGPU caps the lod clamp at 32, where sg spells "unclamped" as FLT_MAX.
    auto const max_lod = s.max_lod > 32.0f ? 32.0f : s.max_lod;
    return WGPUSamplerDescriptor{
        .nextInChain = nullptr,
        .label = WGPU_STRING_VIEW_INIT,
        .addressModeU = to_wgpu_address(s.address_u),
        .addressModeV = to_wgpu_address(s.address_v),
        .addressModeW = to_wgpu_address(s.address_w),
        .magFilter = to_wgpu_filter(s.mag_filter),
        .minFilter = to_wgpu_filter(s.min_filter),
        .mipmapFilter
        = s.mip_filter == sg::sampler_filter::linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest,
        .lodMinClamp = s.min_lod,
        .lodMaxClamp = max_lod < s.min_lod ? s.min_lod : max_lod,
        .compare = s.compare.has_value() ? to_wgpu_compare(s.compare.value()) : WGPUCompareFunction_Undefined,
        // Anisotropy requires every filter to be linear on WebGPU, and is clamped by the implementation past 16.
        .maxAnisotropy
        = u16(all_linear ? (s.max_anisotropy < 1 ? 1 : (s.max_anisotropy > 16 ? 16 : s.max_anisotropy)) : 1),
    };
}

WGPUSamplerBindingType default_sampler_binding_type(sg::sampler const& s)
{
    if (s.compare.has_value())
        return WGPUSamplerBindingType_Comparison;
    auto const filtering = s.min_filter == sg::sampler_filter::linear || s.mag_filter == sg::sampler_filter::linear
                        || s.mip_filter == sg::sampler_filter::linear;
    return filtering ? WGPUSamplerBindingType_Filtering : WGPUSamplerBindingType_NonFiltering;
}

texel_copy_layout texel_copy_layout_of(sg::pixel_format format, tg::vec3i size)
{
    auto const block_extent = isize(sg::format_block_extent(format));
    auto const block_size = isize(sg::format_block_size(format));

    auto out = texel_copy_layout();
    out.row_bytes = (isize(size[0]) + block_extent - 1) / block_extent * block_size;
    out.padded_row = align_up(out.row_bytes, copy_row_alignment);
    out.rows = (isize(size[1]) + block_extent - 1) / block_extent;
    out.images = isize(size[2]);
    out.packed_bytes = out.row_bytes * out.rows * out.images;
    out.staged_bytes = out.rows * out.images == 0 ? 0 : out.padded_row * (out.rows * out.images - 1) + out.row_bytes;
    return out;
}
} // namespace sg::backend::webgpu
