#include <shaped-graphics/backends/dx12/dx12_format.hh>

namespace sg::backend::dx12
{
DXGI_FORMAT to_dxgi_format(sg::pixel_format format)
{
    switch (format)
    {
    case sg::pixel_format::undefined:
        return DXGI_FORMAT_UNKNOWN;

    case sg::pixel_format::r8_unorm:
        return DXGI_FORMAT_R8_UNORM;
    case sg::pixel_format::r8_snorm:
        return DXGI_FORMAT_R8_SNORM;
    case sg::pixel_format::r8_uint:
        return DXGI_FORMAT_R8_UINT;
    case sg::pixel_format::r8_sint:
        return DXGI_FORMAT_R8_SINT;
    case sg::pixel_format::rg8_unorm:
        return DXGI_FORMAT_R8G8_UNORM;
    case sg::pixel_format::rg8_snorm:
        return DXGI_FORMAT_R8G8_SNORM;
    case sg::pixel_format::rg8_uint:
        return DXGI_FORMAT_R8G8_UINT;
    case sg::pixel_format::rg8_sint:
        return DXGI_FORMAT_R8G8_SINT;
    case sg::pixel_format::rgba8_unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case sg::pixel_format::rgba8_snorm:
        return DXGI_FORMAT_R8G8B8A8_SNORM;
    case sg::pixel_format::rgba8_uint:
        return DXGI_FORMAT_R8G8B8A8_UINT;
    case sg::pixel_format::rgba8_sint:
        return DXGI_FORMAT_R8G8B8A8_SINT;
    case sg::pixel_format::rgba8_unorm_srgb:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case sg::pixel_format::bgra8_unorm:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case sg::pixel_format::bgra8_unorm_srgb:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    case sg::pixel_format::r16_float:
        return DXGI_FORMAT_R16_FLOAT;
    case sg::pixel_format::r16_uint:
        return DXGI_FORMAT_R16_UINT;
    case sg::pixel_format::r16_sint:
        return DXGI_FORMAT_R16_SINT;
    case sg::pixel_format::rg16_float:
        return DXGI_FORMAT_R16G16_FLOAT;
    case sg::pixel_format::rg16_uint:
        return DXGI_FORMAT_R16G16_UINT;
    case sg::pixel_format::rg16_sint:
        return DXGI_FORMAT_R16G16_SINT;
    case sg::pixel_format::rgba16_float:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case sg::pixel_format::rgba16_uint:
        return DXGI_FORMAT_R16G16B16A16_UINT;
    case sg::pixel_format::rgba16_sint:
        return DXGI_FORMAT_R16G16B16A16_SINT;

    case sg::pixel_format::r32_float:
        return DXGI_FORMAT_R32_FLOAT;
    case sg::pixel_format::r32_uint:
        return DXGI_FORMAT_R32_UINT;
    case sg::pixel_format::r32_sint:
        return DXGI_FORMAT_R32_SINT;
    case sg::pixel_format::rg32_float:
        return DXGI_FORMAT_R32G32_FLOAT;
    case sg::pixel_format::rg32_uint:
        return DXGI_FORMAT_R32G32_UINT;
    case sg::pixel_format::rg32_sint:
        return DXGI_FORMAT_R32G32_SINT;
    case sg::pixel_format::rgba32_float:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case sg::pixel_format::rgba32_uint:
        return DXGI_FORMAT_R32G32B32A32_UINT;
    case sg::pixel_format::rgba32_sint:
        return DXGI_FORMAT_R32G32B32A32_SINT;

    case sg::pixel_format::rgb10a2_unorm:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case sg::pixel_format::rg11b10_float:
        return DXGI_FORMAT_R11G11B10_FLOAT;

    case sg::pixel_format::depth16_unorm:
        return DXGI_FORMAT_D16_UNORM;
    case sg::pixel_format::depth32_float:
        return DXGI_FORMAT_D32_FLOAT;
    case sg::pixel_format::depth32_float_stencil8:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

    case sg::pixel_format::bc1_rgba_unorm:
        return DXGI_FORMAT_BC1_UNORM;
    case sg::pixel_format::bc1_rgba_unorm_srgb:
        return DXGI_FORMAT_BC1_UNORM_SRGB;
    case sg::pixel_format::bc2_unorm:
        return DXGI_FORMAT_BC2_UNORM;
    case sg::pixel_format::bc2_unorm_srgb:
        return DXGI_FORMAT_BC2_UNORM_SRGB;
    case sg::pixel_format::bc3_unorm:
        return DXGI_FORMAT_BC3_UNORM;
    case sg::pixel_format::bc3_unorm_srgb:
        return DXGI_FORMAT_BC3_UNORM_SRGB;
    case sg::pixel_format::bc4_r_unorm:
        return DXGI_FORMAT_BC4_UNORM;
    case sg::pixel_format::bc4_r_snorm:
        return DXGI_FORMAT_BC4_SNORM;
    case sg::pixel_format::bc5_rg_unorm:
        return DXGI_FORMAT_BC5_UNORM;
    case sg::pixel_format::bc5_rg_snorm:
        return DXGI_FORMAT_BC5_SNORM;
    case sg::pixel_format::bc6h_rgb_ufloat:
        return DXGI_FORMAT_BC6H_UF16;
    case sg::pixel_format::bc6h_rgb_sfloat:
        return DXGI_FORMAT_BC6H_SF16;
    case sg::pixel_format::bc7_rgba_unorm:
        return DXGI_FORMAT_BC7_UNORM;
    case sg::pixel_format::bc7_rgba_unorm_srgb:
        return DXGI_FORMAT_BC7_UNORM_SRGB;
    }

    CC_ASSERT(false, "unhandled pixel_format in to_dxgi_format");
    return DXGI_FORMAT_UNKNOWN;
}
} // namespace sg::backend::dx12
