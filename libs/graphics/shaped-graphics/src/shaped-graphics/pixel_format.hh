#pragma once

#include <shaped-graphics/fwd.hh>

/// The texel formats sg textures speak in. Deliberately restrictive: every value has a direct
/// equivalent in each realistic backend (DX12 / Vulkan / Metal / WebGPU). Formats that are
/// backend-specific, mobile-only, or absent somewhere are left out until a concrete need plus a
/// per-backend capability query justify them (see libs/graphics/shaped-graphics/docs/concepts/textures.md).

namespace sg
{
/// A GPU texel format. `undefined` is the null value (no format). Block-compressed (BC) formats are
/// included but are a *runtime* capability everywhere (Vk `textureCompressionBC`, WGPU
/// `texture-compression-bc`, Metal `supportsBCTextureCompression`) — the enumerant always maps, but a
/// backend may still reject it on a given adapter until a capability query gates its use.
enum class pixel_format : u16
{
    undefined,

    // 8-bit
    r8_unorm,         // DX12 R8_UNORM      / Vk R8_UNORM
    r8_snorm,         // DX12 R8_SNORM      / Vk R8_SNORM
    r8_uint,          // DX12 R8_UINT       / Vk R8_UINT
    r8_sint,          // DX12 R8_SINT       / Vk R8_SINT
    rg8_unorm,        // DX12 R8G8_UNORM    / Vk R8G8_UNORM
    rg8_snorm,        // DX12 R8G8_SNORM    / Vk R8G8_SNORM
    rg8_uint,         // DX12 R8G8_UINT     / Vk R8G8_UINT
    rg8_sint,         // DX12 R8G8_SINT     / Vk R8G8_SINT
    rgba8_unorm,      // DX12 R8G8B8A8_UNORM      / Vk R8G8B8A8_UNORM
    rgba8_snorm,      // DX12 R8G8B8A8_SNORM      / Vk R8G8B8A8_SNORM
    rgba8_uint,       // DX12 R8G8B8A8_UINT       / Vk R8G8B8A8_UINT
    rgba8_sint,       // DX12 R8G8B8A8_SINT       / Vk R8G8B8A8_SINT
    rgba8_unorm_srgb, // DX12 R8G8B8A8_UNORM_SRGB / Vk R8G8B8A8_SRGB
    bgra8_unorm,      // DX12 B8G8R8A8_UNORM      / Vk B8G8R8A8_UNORM
    bgra8_unorm_srgb, // DX12 B8G8R8A8_UNORM_SRGB / Vk B8G8R8A8_SRGB

    // 16-bit (float/int only — 16-bit *norm* is not WebGPU-core)
    r16_float,    // DX12 R16_FLOAT        / Vk R16_SFLOAT
    r16_uint,     // DX12 R16_UINT         / Vk R16_UINT
    r16_sint,     // DX12 R16_SINT         / Vk R16_SINT
    rg16_float,   // DX12 R16G16_FLOAT     / Vk R16G16_SFLOAT
    rg16_uint,    // DX12 R16G16_UINT      / Vk R16G16_UINT
    rg16_sint,    // DX12 R16G16_SINT      / Vk R16G16_SINT
    rgba16_float, // DX12 R16G16B16A16_FLOAT / Vk R16G16B16A16_SFLOAT
    rgba16_uint,  // DX12 R16G16B16A16_UINT  / Vk R16G16B16A16_UINT
    rgba16_sint,  // DX12 R16G16B16A16_SINT  / Vk R16G16B16A16_SINT

    // 32-bit
    r32_float,    // DX12 R32_FLOAT        / Vk R32_SFLOAT
    r32_uint,     // DX12 R32_UINT         / Vk R32_UINT
    r32_sint,     // DX12 R32_SINT         / Vk R32_SINT
    rg32_float,   // DX12 R32G32_FLOAT     / Vk R32G32_SFLOAT
    rg32_uint,    // DX12 R32G32_UINT      / Vk R32G32_UINT
    rg32_sint,    // DX12 R32G32_SINT      / Vk R32G32_SINT
    rgba32_float, // DX12 R32G32B32A32_FLOAT / Vk R32G32B32A32_SFLOAT
    rgba32_uint,  // DX12 R32G32B32A32_UINT  / Vk R32G32B32A32_UINT
    rgba32_sint,  // DX12 R32G32B32A32_SINT  / Vk R32G32B32A32_SINT

    // packed
    rgb10a2_unorm, // DX12 R10G10B10A2_UNORM / Vk A2B10G10R10_UNORM_PACK32
    rg11b10_float, // DX12 R11G11B10_FLOAT   / Vk B10G11R11_UFLOAT_PACK32

    // depth / depth-stencil
    depth16_unorm,          // DX12 D16_UNORM          / Vk D16_UNORM
    depth32_float,          // DX12 D32_FLOAT          / Vk D32_SFLOAT
    depth32_float_stencil8, // DX12 D32_FLOAT_S8X24_UINT / Vk D32_SFLOAT_S8_UINT

    // BC block-compressed (4x4 blocks; runtime feature-gated, see above)
    bc1_rgba_unorm,      // DX12 BC1_UNORM      / Vk BC1_RGBA_UNORM_BLOCK
    bc1_rgba_unorm_srgb, // DX12 BC1_UNORM_SRGB / Vk BC1_RGBA_SRGB_BLOCK
    bc2_unorm,           // DX12 BC2_UNORM      / Vk BC2_UNORM_BLOCK
    bc2_unorm_srgb,      // DX12 BC2_UNORM_SRGB / Vk BC2_SRGB_BLOCK
    bc3_unorm,           // DX12 BC3_UNORM      / Vk BC3_UNORM_BLOCK
    bc3_unorm_srgb,      // DX12 BC3_UNORM_SRGB / Vk BC3_SRGB_BLOCK
    bc4_r_unorm,         // DX12 BC4_UNORM      / Vk BC4_UNORM_BLOCK
    bc4_r_snorm,         // DX12 BC4_SNORM      / Vk BC4_SNORM_BLOCK
    bc5_rg_unorm,        // DX12 BC5_UNORM      / Vk BC5_UNORM_BLOCK
    bc5_rg_snorm,        // DX12 BC5_SNORM      / Vk BC5_SNORM_BLOCK
    bc6h_rgb_ufloat,     // DX12 BC6H_UF16      / Vk BC6H_UFLOAT_BLOCK
    bc6h_rgb_sfloat,     // DX12 BC6H_SF16      / Vk BC6H_SFLOAT_BLOCK
    bc7_rgba_unorm,      // DX12 BC7_UNORM      / Vk BC7_UNORM_BLOCK
    bc7_rgba_unorm_srgb, // DX12 BC7_UNORM_SRGB / Vk BC7_SRGB_BLOCK
};

/// True for the depth (and depth-stencil) formats.
[[nodiscard]] constexpr bool is_depth_format(pixel_format f)
{
    return f == pixel_format::depth16_unorm || f == pixel_format::depth32_float
        || f == pixel_format::depth32_float_stencil8;
}

/// True for formats that carry a stencil plane.
[[nodiscard]] constexpr bool has_stencil(pixel_format f)
{
    return f == pixel_format::depth32_float_stencil8;
}

/// True for a combined depth+stencil format (a two-plane subresource).
[[nodiscard]] constexpr bool is_depth_stencil_format(pixel_format f)
{
    return is_depth_format(f) && has_stencil(f);
}

/// Number of aspect planes a format exposes as subresources: 2 for a combined depth+stencil format
/// (depth and stencil are separately tracked planes), 1 otherwise.
[[nodiscard]] constexpr int format_aspect_count(pixel_format f)
{
    return is_depth_stencil_format(f) ? 2 : 1;
}

/// True for the BC block-compressed formats (4x4 texel blocks).
[[nodiscard]] constexpr bool is_compressed_format(pixel_format f)
{
    switch (f)
    {
    case pixel_format::bc1_rgba_unorm:
    case pixel_format::bc1_rgba_unorm_srgb:
    case pixel_format::bc2_unorm:
    case pixel_format::bc2_unorm_srgb:
    case pixel_format::bc3_unorm:
    case pixel_format::bc3_unorm_srgb:
    case pixel_format::bc4_r_unorm:
    case pixel_format::bc4_r_snorm:
    case pixel_format::bc5_rg_unorm:
    case pixel_format::bc5_rg_snorm:
    case pixel_format::bc6h_rgb_ufloat:
    case pixel_format::bc6h_rgb_sfloat:
    case pixel_format::bc7_rgba_unorm:
    case pixel_format::bc7_rgba_unorm_srgb:
        return true;
    default:
        return false;
    }
}

/// True for a color format usable as a render target: any non-depth, non-compressed
/// format. A coarse capability check — a given adapter may still restrict blending on some of these.
[[nodiscard]] constexpr bool is_render_target_format(pixel_format f)
{
    return f != pixel_format::undefined && !is_depth_format(f) && !is_compressed_format(f);
}

/// Edge length of a format's addressable block: 1 for uncompressed (one texel), 4 for BC.
[[nodiscard]] constexpr int format_block_extent(pixel_format f)
{
    return is_compressed_format(f) ? 4 : 1;
}

/// Bytes occupied by one block — one texel for uncompressed, one 4x4 block for BC. `undefined` is 0.
[[nodiscard]] constexpr int format_block_size(pixel_format f)
{
    switch (f)
    {
    case pixel_format::undefined:
        return 0;

    case pixel_format::r8_unorm:
    case pixel_format::r8_snorm:
    case pixel_format::r8_uint:
    case pixel_format::r8_sint:
        return 1;

    case pixel_format::rg8_unorm:
    case pixel_format::rg8_snorm:
    case pixel_format::rg8_uint:
    case pixel_format::rg8_sint:
    case pixel_format::r16_float:
    case pixel_format::r16_uint:
    case pixel_format::r16_sint:
    case pixel_format::depth16_unorm:
        return 2;

    case pixel_format::rgba8_unorm:
    case pixel_format::rgba8_snorm:
    case pixel_format::rgba8_uint:
    case pixel_format::rgba8_sint:
    case pixel_format::rgba8_unorm_srgb:
    case pixel_format::bgra8_unorm:
    case pixel_format::bgra8_unorm_srgb:
    case pixel_format::rg16_float:
    case pixel_format::rg16_uint:
    case pixel_format::rg16_sint:
    case pixel_format::r32_float:
    case pixel_format::r32_uint:
    case pixel_format::r32_sint:
    case pixel_format::rgb10a2_unorm:
    case pixel_format::rg11b10_float:
    case pixel_format::depth32_float:
        return 4;

    case pixel_format::rgba16_float:
    case pixel_format::rgba16_uint:
    case pixel_format::rgba16_sint:
    case pixel_format::rg32_float:
    case pixel_format::rg32_uint:
    case pixel_format::rg32_sint:
    case pixel_format::depth32_float_stencil8: // D32 + S8X24 occupies 8 bytes
    case pixel_format::bc1_rgba_unorm:         // BC1 / BC4: 8-byte blocks
    case pixel_format::bc1_rgba_unorm_srgb:
    case pixel_format::bc4_r_unorm:
    case pixel_format::bc4_r_snorm:
        return 8;

    case pixel_format::rgba32_float:
    case pixel_format::rgba32_uint:
    case pixel_format::rgba32_sint:
    case pixel_format::bc2_unorm: // remaining BC: 16-byte blocks
    case pixel_format::bc2_unorm_srgb:
    case pixel_format::bc3_unorm:
    case pixel_format::bc3_unorm_srgb:
    case pixel_format::bc5_rg_unorm:
    case pixel_format::bc5_rg_snorm:
    case pixel_format::bc6h_rgb_ufloat:
    case pixel_format::bc6h_rgb_sfloat:
    case pixel_format::bc7_rgba_unorm:
    case pixel_format::bc7_rgba_unorm_srgb:
        return 16;
    }
    return 0;
}
} // namespace sg
