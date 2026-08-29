// vulkan_texture: GPU texture (VkImage) creation and teardown.
// The texture type itself is header-only, so the create path, the format/usage maps and the destructor live here.

#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_format.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>

namespace sg::backend::vulkan
{

vulkan_texture::~vulkan_texture()
{
    // Stage the GPU handles and finalizers for deletion once the current epoch retires.
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
    // Validate the shape contract before any fallible GPU work, so a bad desc asserts at the entry point rather than surfacing as a driver error.
    desc.assert_valid();

    // TEMPORARY: dedicated allocations only.
    // Placement into a memory_heap needs vkBindImageMemory at an offset into a shared VkDeviceMemory, same status as vulkan_buffer.
    CC_ASSERT(alloc.is_dedicated(), "placed textures (non-null memory_heap) not implemented yet");

    // Extent + layer count derived from the shape: depth only for 3D; a cube is 6 layers per cube.
    u32 const height = desc.dimension == sg::texture_dimension::d1 ? 1u : u32(desc.height);
    u32 const depth = desc.dimension == sg::texture_dimension::d3 ? u32(desc.depth) : 1u;
    int layers = desc.array_layers.value_or(1);
    if (desc.is_cube)
        layers *= 6;

    auto const image_info = VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VkImageCreateFlags(desc.is_cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
        .imageType = to_vk_image_type(desc.dimension),
        .format = to_vk_format(desc.format),
        .extent = {u32(desc.width), height, depth},
        .mipLevels = u32(desc.mip_levels),
        .arrayLayers = u32(layers),
        .samples = VkSampleCountFlagBits(desc.sample_count), // enum values equal the sample counts
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = to_vk_image_usage(desc.usage),
        // TODO: the streaming usages need VK_SHARING_MODE_CONCURRENT over the graphics + transfer
        // families — EXCLUSIVE cannot express two families holding a resource at once, and ownership
        // transfer serializes the very concurrency streaming exists to allow.
        // Vulkan is the strict backend here: dx12 needs a flag only for a region inside a subresource.
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage image = VK_NULL_HANDLE;
    if (VkResult r = vkCreateImage(_device, &image_info, nullptr, &image); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateImage failed");

    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(_device, image, &req);

    u32 const type = find_memory_type(u32(req.memoryTypeBits), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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

    return vulkan_texture_handle(std::make_shared<vulkan_texture>(*this, current_epoch(), desc, image, memory));
}
} // namespace sg::backend::vulkan
