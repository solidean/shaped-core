// vulkan_texture: GPU texture (VkImage) creation and teardown. The texture type is header-only (ctor +
// fields); the allocating create path, the format/usage maps, and the destructor live here.

#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>

namespace sg::backend::vulkan
{
namespace
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

VkImageUsageFlags to_vk_image_usage(sg::texture_usage u)
{
    VkImageUsageFlags flags = 0;
    if (sg::has_flag(u, sg::texture_usage::copy_src))
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (sg::has_flag(u, sg::texture_usage::copy_dst))
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (sg::has_flag(u, sg::texture_usage::sampled))
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (sg::has_flag(u, sg::texture_usage::storage))
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (sg::has_flag(u, sg::texture_usage::render_target))
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (sg::has_flag(u, sg::texture_usage::depth_stencil))
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    // Vulkan rejects a zero-usage image; a usage-less texture keeps a benign SAMPLED bit so the handle
    // stays valid (mirrors the buffer path's transfer-dst fallback).
    if (flags == 0)
        flags = VK_IMAGE_USAGE_SAMPLED_BIT;
    return flags;
}
} // namespace

vulkan_texture::~vulkan_texture()
{
    // Stage the GPU handles + finalizers for deletion once the current epoch retires.
    if (_image != VK_NULL_HANDLE || _memory != VK_NULL_HANDLE || !_finalizers.empty())
    {
        vulkan_expiring_resource expiring;
        expiring.image = _image;
        expiring.memory = _memory;
        expiring.finalizers = cc::move(_finalizers);
        _ctx.schedule_deferred_deletion(cc::move(expiring));
    }
}

cc::result<vulkan_texture_handle> vulkan_context::create_vulkan_texture(sg::texture_description const& desc,
                                                                        sg::allocation_info const& alloc)
{
    // TEMPORARY: dedicated allocations only. Placement into a memory_heap (vkBindImageMemory at an offset
    // into a shared VkDeviceMemory) is not wired up yet — same status as vulkan_buffer.
    CC_ASSERT(alloc.is_dedicated(), "placed textures (non-null memory_heap) not implemented yet");

    // Extent + layer count derived from the shape: depth only for 3D; a cube is 6 layers per cube.
    cc::u32 const height = desc.dimension == sg::texture_dimension::d1 ? 1u : cc::u32(desc.height);
    cc::u32 const depth = desc.dimension == sg::texture_dimension::d3 ? cc::u32(desc.depth) : 1u;
    int layers = desc.array_layers.value_or(1);
    if (desc.is_cube)
        layers *= 6;

    auto const image_info = VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VkImageCreateFlags(desc.is_cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
        .imageType = to_vk_image_type(desc.dimension),
        .format = to_vk_format(desc.format),
        .extent = {cc::u32(desc.width), height, depth},
        .mipLevels = cc::u32(desc.mip_levels),
        .arrayLayers = cc::u32(layers),
        .samples = VkSampleCountFlagBits(desc.sample_count), // enum values equal the sample counts
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = to_vk_image_usage(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage image = VK_NULL_HANDLE;
    if (VkResult r = vkCreateImage(_device, &image_info, nullptr, &image); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateImage failed");

    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(_device, image, &req);

    // GPU-resident: sg exposes no host-visible textures.
    cc::u32 const type = find_memory_type(cc::u32(req.memoryTypeBits), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        vkDestroyImage(_device, image, nullptr);
        return cc::error("no device-local memory type for texture");
    }

    auto const alloc_info = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (VkResult r = vkAllocateMemory(_device, &alloc_info, nullptr, &memory); r != VK_SUCCESS)
    {
        vkDestroyImage(_device, image, nullptr);
        return vulkan_error(r, "vkAllocateMemory (texture) failed");
    }

    if (VkResult r = vkBindImageMemory(_device, image, memory, 0); r != VK_SUCCESS)
    {
        vkFreeMemory(_device, memory, nullptr);
        vkDestroyImage(_device, image, nullptr);
        return vulkan_error(r, "vkBindImageMemory failed");
    }

    return std::make_shared<vulkan_texture>(*this, current_epoch(), desc, image, memory);
}
} // namespace sg::backend::vulkan
