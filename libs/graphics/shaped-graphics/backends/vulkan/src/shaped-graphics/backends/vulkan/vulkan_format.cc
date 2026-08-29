#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_format.hh>

namespace sg::backend::vulkan
{
VkFormat to_vk_format(sg::pixel_format f)
{
    switch (f)
    {
    case sg::pixel_format::undefined:
        return VK_FORMAT_UNDEFINED;

    case sg::pixel_format::r8_unorm:
        return VK_FORMAT_R8_UNORM;
    case sg::pixel_format::r8_snorm:
        return VK_FORMAT_R8_SNORM;
    case sg::pixel_format::r8_uint:
        return VK_FORMAT_R8_UINT;
    case sg::pixel_format::r8_sint:
        return VK_FORMAT_R8_SINT;
    case sg::pixel_format::rg8_unorm:
        return VK_FORMAT_R8G8_UNORM;
    case sg::pixel_format::rg8_snorm:
        return VK_FORMAT_R8G8_SNORM;
    case sg::pixel_format::rg8_uint:
        return VK_FORMAT_R8G8_UINT;
    case sg::pixel_format::rg8_sint:
        return VK_FORMAT_R8G8_SINT;
    case sg::pixel_format::rgba8_unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case sg::pixel_format::rgba8_snorm:
        return VK_FORMAT_R8G8B8A8_SNORM;
    case sg::pixel_format::rgba8_uint:
        return VK_FORMAT_R8G8B8A8_UINT;
    case sg::pixel_format::rgba8_sint:
        return VK_FORMAT_R8G8B8A8_SINT;
    case sg::pixel_format::rgba8_unorm_srgb:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case sg::pixel_format::bgra8_unorm:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case sg::pixel_format::bgra8_unorm_srgb:
        return VK_FORMAT_B8G8R8A8_SRGB;

    case sg::pixel_format::r16_float:
        return VK_FORMAT_R16_SFLOAT;
    case sg::pixel_format::r16_uint:
        return VK_FORMAT_R16_UINT;
    case sg::pixel_format::r16_sint:
        return VK_FORMAT_R16_SINT;
    case sg::pixel_format::rg16_float:
        return VK_FORMAT_R16G16_SFLOAT;
    case sg::pixel_format::rg16_uint:
        return VK_FORMAT_R16G16_UINT;
    case sg::pixel_format::rg16_sint:
        return VK_FORMAT_R16G16_SINT;
    case sg::pixel_format::rgba16_float:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case sg::pixel_format::rgba16_uint:
        return VK_FORMAT_R16G16B16A16_UINT;
    case sg::pixel_format::rgba16_sint:
        return VK_FORMAT_R16G16B16A16_SINT;

    case sg::pixel_format::r32_float:
        return VK_FORMAT_R32_SFLOAT;
    case sg::pixel_format::r32_uint:
        return VK_FORMAT_R32_UINT;
    case sg::pixel_format::r32_sint:
        return VK_FORMAT_R32_SINT;
    case sg::pixel_format::rg32_float:
        return VK_FORMAT_R32G32_SFLOAT;
    case sg::pixel_format::rg32_uint:
        return VK_FORMAT_R32G32_UINT;
    case sg::pixel_format::rg32_sint:
        return VK_FORMAT_R32G32_SINT;
    case sg::pixel_format::rgba32_float:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case sg::pixel_format::rgba32_uint:
        return VK_FORMAT_R32G32B32A32_UINT;
    case sg::pixel_format::rgba32_sint:
        return VK_FORMAT_R32G32B32A32_SINT;

    case sg::pixel_format::rgb10a2_unorm:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case sg::pixel_format::rg11b10_float:
        return VK_FORMAT_B10G11R11_UFLOAT_PACK32;

    case sg::pixel_format::depth16_unorm:
        return VK_FORMAT_D16_UNORM;
    case sg::pixel_format::depth32_float:
        return VK_FORMAT_D32_SFLOAT;
    case sg::pixel_format::depth32_float_stencil8:
        return VK_FORMAT_D32_SFLOAT_S8_UINT;

    case sg::pixel_format::bc1_rgba_unorm:
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case sg::pixel_format::bc1_rgba_unorm_srgb:
        return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case sg::pixel_format::bc2_unorm:
        return VK_FORMAT_BC2_UNORM_BLOCK;
    case sg::pixel_format::bc2_unorm_srgb:
        return VK_FORMAT_BC2_SRGB_BLOCK;
    case sg::pixel_format::bc3_unorm:
        return VK_FORMAT_BC3_UNORM_BLOCK;
    case sg::pixel_format::bc3_unorm_srgb:
        return VK_FORMAT_BC3_SRGB_BLOCK;
    case sg::pixel_format::bc4_r_unorm:
        return VK_FORMAT_BC4_UNORM_BLOCK;
    case sg::pixel_format::bc4_r_snorm:
        return VK_FORMAT_BC4_SNORM_BLOCK;
    case sg::pixel_format::bc5_rg_unorm:
        return VK_FORMAT_BC5_UNORM_BLOCK;
    case sg::pixel_format::bc5_rg_snorm:
        return VK_FORMAT_BC5_SNORM_BLOCK;
    case sg::pixel_format::bc6h_rgb_ufloat:
        return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case sg::pixel_format::bc6h_rgb_sfloat:
        return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case sg::pixel_format::bc7_rgba_unorm:
        return VK_FORMAT_BC7_UNORM_BLOCK;
    case sg::pixel_format::bc7_rgba_unorm_srgb:
        return VK_FORMAT_BC7_SRGB_BLOCK;
    }

    CC_ASSERT(false, "unhandled pixel_format in to_vk_format");
    return VK_FORMAT_UNDEFINED;
}

VkImageType to_vk_image_type(sg::texture_dimension d)
{
    switch (d)
    {
    case sg::texture_dimension::d1:
        return VK_IMAGE_TYPE_1D;
    case sg::texture_dimension::d2:
        return VK_IMAGE_TYPE_2D;
    case sg::texture_dimension::d3:
        return VK_IMAGE_TYPE_3D;
    }
    return VK_IMAGE_TYPE_2D;
}

VkImageUsageFlags to_vk_image_usage(sg::texture_usages u)
{
    VkImageUsageFlags flags = 0;
    if (u.has(sg::texture_usage::copy_src))
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (u.has(sg::texture_usage::copy_dst))
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (u.has(sg::texture_usage::readonly_texture))
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (u.has(sg::texture_usage::readwrite_texture))
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (u.has(sg::texture_usage::render_target))
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (u.has(sg::texture_usage::depth_stencil))
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    // Vulkan rejects a zero-usage image, so a usage-less texture keeps a benign SAMPLED bit and stays valid.
    // Same shape as the buffer path's transfer-dst fallback.
    if (flags == 0)
        flags = VK_IMAGE_USAGE_SAMPLED_BIT;
    return flags;
}

VkBufferUsageFlags to_vk_buffer_usage(sg::buffer_usages usage)
{
    VkBufferUsageFlags flags = 0;
    if (usage.has(sg::buffer_usage::copy_src))
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (usage.has(sg::buffer_usage::copy_dst))
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (usage.has(sg::buffer_usage::vertex_buffer))
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (usage.has(sg::buffer_usage::index_buffer))
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (usage.has(sg::buffer_usage::uniform_buffer))
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    // Vulkan does not distinguish read-only from read-write storage at the usage-bit level — that is a descriptor/access concern.
    // So both map to the same STORAGE_BUFFER_BIT.
    if (usage.has(sg::buffer_usage::readonly_buffer) || usage.has(sg::buffer_usage::readwrite_buffer))
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    // Vulkan rejects a zero-usage buffer, so a usage-less non-empty buffer keeps a benign transfer-dst bit and stays valid.
    if (flags == 0)
        flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return flags;
}
} // namespace sg::backend::vulkan
