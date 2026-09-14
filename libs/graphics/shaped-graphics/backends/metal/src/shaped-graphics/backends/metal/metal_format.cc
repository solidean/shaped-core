#include "metal_format.hh"

#include <shaped-graphics/types.hh>

namespace sg::backend::metal
{
MTL::PixelFormat pixel_format_of(sg::pixel_format format)
{
    switch (format)
    {
    case sg::pixel_format::undefined:
        return MTL::PixelFormatInvalid;

    case sg::pixel_format::r8_unorm:
        return MTL::PixelFormatR8Unorm;
    case sg::pixel_format::r8_snorm:
        return MTL::PixelFormatR8Snorm;
    case sg::pixel_format::r8_uint:
        return MTL::PixelFormatR8Uint;
    case sg::pixel_format::r8_sint:
        return MTL::PixelFormatR8Sint;

    case sg::pixel_format::rg8_unorm:
        return MTL::PixelFormatRG8Unorm;
    case sg::pixel_format::rg8_snorm:
        return MTL::PixelFormatRG8Snorm;
    case sg::pixel_format::rg8_uint:
        return MTL::PixelFormatRG8Uint;
    case sg::pixel_format::rg8_sint:
        return MTL::PixelFormatRG8Sint;

    case sg::pixel_format::rgba8_unorm:
        return MTL::PixelFormatRGBA8Unorm;
    case sg::pixel_format::rgba8_snorm:
        return MTL::PixelFormatRGBA8Snorm;
    case sg::pixel_format::rgba8_uint:
        return MTL::PixelFormatRGBA8Uint;
    case sg::pixel_format::rgba8_sint:
        return MTL::PixelFormatRGBA8Sint;
    case sg::pixel_format::rgba8_unorm_srgb:
        return MTL::PixelFormatRGBA8Unorm_sRGB;
    case sg::pixel_format::bgra8_unorm:
        return MTL::PixelFormatBGRA8Unorm;
    case sg::pixel_format::bgra8_unorm_srgb:
        return MTL::PixelFormatBGRA8Unorm_sRGB;

    case sg::pixel_format::r16_float:
        return MTL::PixelFormatR16Float;
    case sg::pixel_format::r16_uint:
        return MTL::PixelFormatR16Uint;
    case sg::pixel_format::r16_sint:
        return MTL::PixelFormatR16Sint;
    case sg::pixel_format::rg16_float:
        return MTL::PixelFormatRG16Float;
    case sg::pixel_format::rg16_uint:
        return MTL::PixelFormatRG16Uint;
    case sg::pixel_format::rg16_sint:
        return MTL::PixelFormatRG16Sint;
    case sg::pixel_format::rgba16_float:
        return MTL::PixelFormatRGBA16Float;
    case sg::pixel_format::rgba16_uint:
        return MTL::PixelFormatRGBA16Uint;
    case sg::pixel_format::rgba16_sint:
        return MTL::PixelFormatRGBA16Sint;

    case sg::pixel_format::r32_float:
        return MTL::PixelFormatR32Float;
    case sg::pixel_format::r32_uint:
        return MTL::PixelFormatR32Uint;
    case sg::pixel_format::r32_sint:
        return MTL::PixelFormatR32Sint;
    case sg::pixel_format::rg32_float:
        return MTL::PixelFormatRG32Float;
    case sg::pixel_format::rg32_uint:
        return MTL::PixelFormatRG32Uint;
    case sg::pixel_format::rg32_sint:
        return MTL::PixelFormatRG32Sint;
    case sg::pixel_format::rgba32_float:
        return MTL::PixelFormatRGBA32Float;
    case sg::pixel_format::rgba32_uint:
        return MTL::PixelFormatRGBA32Uint;
    case sg::pixel_format::rgba32_sint:
        return MTL::PixelFormatRGBA32Sint;

    case sg::pixel_format::rgb10a2_unorm:
        return MTL::PixelFormatRGB10A2Unorm;
    case sg::pixel_format::rg11b10_float:
        return MTL::PixelFormatRG11B10Float;

    case sg::pixel_format::depth16_unorm:
        return MTL::PixelFormatDepth16Unorm;
    case sg::pixel_format::depth32_float:
        return MTL::PixelFormatDepth32Float;
    case sg::pixel_format::depth32_float_stencil8:
        return MTL::PixelFormatDepth32Float_Stencil8;

    // The BC family.
    // Apple silicon has supported it since M1, which is at or below this backend's floor, so these need no capability
    // probe — a device that could not decode them could not run the backend at all.
    case sg::pixel_format::bc1_rgba_unorm:
        return MTL::PixelFormatBC1_RGBA;
    case sg::pixel_format::bc1_rgba_unorm_srgb:
        return MTL::PixelFormatBC1_RGBA_sRGB;
    case sg::pixel_format::bc2_unorm:
        return MTL::PixelFormatBC2_RGBA;
    case sg::pixel_format::bc2_unorm_srgb:
        return MTL::PixelFormatBC2_RGBA_sRGB;
    case sg::pixel_format::bc3_unorm:
        return MTL::PixelFormatBC3_RGBA;
    case sg::pixel_format::bc3_unorm_srgb:
        return MTL::PixelFormatBC3_RGBA_sRGB;
    case sg::pixel_format::bc4_r_unorm:
        return MTL::PixelFormatBC4_RUnorm;
    case sg::pixel_format::bc4_r_snorm:
        return MTL::PixelFormatBC4_RSnorm;
    case sg::pixel_format::bc5_rg_unorm:
        return MTL::PixelFormatBC5_RGUnorm;
    case sg::pixel_format::bc5_rg_snorm:
        return MTL::PixelFormatBC5_RGSnorm;
    case sg::pixel_format::bc6h_rgb_ufloat:
        return MTL::PixelFormatBC6H_RGBUfloat;
    case sg::pixel_format::bc6h_rgb_sfloat:
        return MTL::PixelFormatBC6H_RGBFloat;
    case sg::pixel_format::bc7_rgba_unorm:
        return MTL::PixelFormatBC7_RGBAUnorm;
    case sg::pixel_format::bc7_rgba_unorm_srgb:
        return MTL::PixelFormatBC7_RGBAUnorm_sRGB;
    }

    return MTL::PixelFormatInvalid;
}

MTL::TextureType texture_type_of(sg::texture_dimension dimension, bool is_array, bool is_cube, bool is_multisampled)
{
    // Metal folds dimension, array-ness, cube-ness and multisampling into one enum where sg keeps them orthogonal, so
    // this is where the product is taken — and where a combination Metal has no name for shows up as invalid rather
    // than as a silently wrong type.
    if (is_cube)
        return is_array ? MTL::TextureTypeCubeArray : MTL::TextureTypeCube;

    switch (dimension)
    {
    case sg::texture_dimension::d1:
        return is_array ? MTL::TextureType1DArray : MTL::TextureType1D;

    case sg::texture_dimension::d2:
        if (is_multisampled)
            return is_array ? MTL::TextureType2DMultisampleArray : MTL::TextureType2DMultisample;
        return is_array ? MTL::TextureType2DArray : MTL::TextureType2D;

    case sg::texture_dimension::d3:
        // Metal has no 3D array and no multisampled 3D; sg's description validation rejects both before here.
        return MTL::TextureType3D;
    }

    return MTL::TextureType2D;
}

MTL::TextureUsage texture_usage_of(sg::texture_usages usage)
{
    MTL::TextureUsage out = MTL::TextureUsageUnknown;

    if (usage.has(sg::texture_usage::readonly_texture))
        out |= MTL::TextureUsageShaderRead;
    if (usage.has(sg::texture_usage::readwrite_texture))
        out |= MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite;
    if (usage.has(sg::texture_usage::render_target) || usage.has(sg::texture_usage::depth_stencil))
        out |= MTL::TextureUsageRenderTarget;

    // copy_src and copy_dst map to nothing: Metal has no copy usage bits, and every texture can be either — the same
    // way they are implicit on D3D12.
    return out;
}

texture_staging_layout staging_layout_of(sg::pixel_format format, sg::texture_region const& region)
{
    auto const block_extent = isize(sg::format_block_extent(format));
    auto const block_size = isize(sg::format_block_size(format));

    auto const blocks_x = (isize(region.size[0]) + block_extent - 1) / block_extent;
    auto const blocks_y = (isize(region.size[1]) + block_extent - 1) / block_extent;

    auto const bytes_per_row = blocks_x * block_size;
    auto const bytes_per_image = bytes_per_row * blocks_y;
    return {
        .bytes_per_row = bytes_per_row,
        .bytes_per_image = bytes_per_image,
        .size_in_bytes = bytes_per_image * isize(region.size[2]),
    };
}
} // namespace sg::backend::metal
