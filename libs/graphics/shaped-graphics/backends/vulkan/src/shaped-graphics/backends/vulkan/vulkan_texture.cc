// vulkan_texture: GPU texture (VkImage) creation and teardown.
// The texture type itself is header-only, so the create path, the format/usage maps and the destructor live here.

#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_format.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>

namespace sg::backend::vulkan
{

std::shared_ptr<vulkan_texture> vulkan_context::register_if_transient(std::shared_ptr<vulkan_texture> texture,
                                                                      sg::lifetime_scope scope)
{
    if (scope == sg::lifetime_scope::transient)
        _transient_expiring_textures.lock([&](cc::vector<std::weak_ptr<sg::raw_texture const>>& v)
                                          { v.push_back(texture); });
    return texture;
}

void vulkan_texture::release_storage() const
{
    // Stage the GPU handles and finalizers for deletion once the current epoch retires.
    // Idempotent: the handles are cleared here, so expiry and destruction cannot stage the same ones twice.
    if (_image == VK_NULL_HANDLE && _memory == VK_NULL_HANDLE && _finalizers.empty())
        return;

    vulkan_expiring_resource expiring;
    // A borrowed image is not ours to destroy — its owner outlives this wrapper by contract — so only the finalizers
    // are staged for it.
    if (_owns_image)
        expiring.image = _image;
    expiring.memory = _memory;
    expiring.finalizers = cc::move(_finalizers);

    // The transfer queue may still be copying into or out of this image, which the epoch says nothing about.
    // The same gate vulkan_buffer takes, and for the same reason — see its release_storage.
    auto const upload_pending = _pending_async_upload_value.load(cc::memory_order_acquire);
    auto const stream_pending = _pending_stream_copy_value.load(cc::memory_order_acquire);
    auto const download_pending = _pending_async_download_value.load(cc::memory_order_acquire);
    auto const stream_download_pending = _pending_stream_download_value.load(cc::memory_order_acquire);
    auto const highest_upload = upload_pending > stream_pending ? upload_pending : stream_pending;
    auto const highest_download = download_pending > stream_download_pending ? download_pending : stream_download_pending;
    if (highest_upload != 0 && _upload_group != nullptr)
        expiring.copy_wait = {.group = _upload_group, .value = highest_upload};
    else if (highest_download != 0 && _download_group != nullptr)
        expiring.copy_wait = {.group = _download_group, .value = highest_download};

    _image = VK_NULL_HANDLE;
    _memory = VK_NULL_HANDLE;
    _ctx.schedule_deferred_deletion(cc::move(expiring));
}

void vulkan_texture::on_expired() const
{
    release_storage();
}

vulkan_texture::~vulkan_texture()
{
    release_storage();
} // no-op if expire() already released the storage

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

    // A texture any async transfer can touch is shared CONCURRENTLY between the graphics and transfer families, for
    // the same reason a buffer is: an ownership transfer would serialize the concurrency async transfer provides.
    //
    // It is not free the way a buffer's is — some hardware disables lossless compression for a concurrently-shared
    // image — which is why it is scoped to textures whose usage says a transfer is possible at all, rather than
    // applied to every image.
    u32 const families[2] = {_queue_family_index, _transfer_queue_family};
    bool const shared = has_dedicated_transfer_queue()
                     && desc.usage.has_any(sg::texture_usage::copy_src | sg::texture_usage::copy_dst);

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
        .sharingMode = shared ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = shared ? 2u : 0u,
        .pQueueFamilyIndices = shared ? families : nullptr,
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

    return vulkan_texture_handle(register_if_transient(
        std::make_shared<vulkan_texture>(*this, current_epoch(), desc, image, memory), alloc.scope));
}
} // namespace sg::backend::vulkan
