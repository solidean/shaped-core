#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_format.hh>
#include <shaped-graphics/backends/vulkan/vulkan_memory_heap.hh>
#include <shaped-graphics/resource/raw_texture.hh>

namespace sg::backend::vulkan
{
namespace
{
/// The device-local memory type a textures heap allocates from: one a sampled, storage and color-attachment image takes.
/// Depth and other formats ask their own requirements at placement, and go dedicated where their types exclude it.
u32 texture_memory_type(vulkan_context& ctx)
{
    auto const probe_desc = sg::texture_description{
        .format = sg::pixel_format::rgba8_unorm,
        .dimension = sg::texture_dimension::d2,
        .width = 64,
        .height = 64,
        .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::render_target
               | sg::texture_usage::copy_src | sg::texture_usage::copy_dst,
    };
    auto image = ctx.create_vk_image(probe_desc);
    if (image.has_error())
        return UINT32_MAX;
    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(ctx._device, image.value(), &requirements);
    vkDestroyImage(ctx._device, image.value(), nullptr);
    return ctx.find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}
} // namespace

cc::result<vulkan_memory_heap_handle> vulkan_memory_heap::create(vulkan_context& ctx,
                                                                 isize size_in_bytes,
                                                                 sg::memory_heap_contents contents)
{
    CC_ASSERT(size_in_bytes >= 0, "heap size must be non-negative");
    if (size_in_bytes == 0)
        return vulkan_memory_heap_handle(
            std::make_shared<vulkan_memory_heap>(ctx, VK_NULL_HANDLE, 0, contents, UINT32_MAX));

    if (contents == sg::memory_heap_contents::textures)
    {
        u32 const type = texture_memory_type(ctx);
        if (type == UINT32_MAX)
            return cc::error("no device-local memory type for a textures heap");
        auto const alloc = VkMemoryAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = VkDeviceSize(size_in_bytes),
            .memoryTypeIndex = type,
        };
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (VkResult const r = vkAllocateMemory(ctx._device, &alloc, nullptr, &memory); r != VK_SUCCESS)
            return vulkan_error(r, "vkAllocateMemory (textures heap) failed");
        return vulkan_memory_heap_handle(std::make_shared<vulkan_memory_heap>(ctx, memory, size_in_bytes, contents, type));
    }

    // The memory type has to serve every buffer that will be placed here, and the caller has not said which those
    // are yet.
    // A probe carrying every usage sg can place therefore gives the conservative type mask: memoryTypeBits only ever
    // narrows as usages are added, so a type serving all of them serves any subset.
    constexpr auto every_usage = sg::buffer_usage::copy_src | sg::buffer_usage::copy_dst
                               | sg::buffer_usage::vertex_buffer | sg::buffer_usage::index_buffer
                               | sg::buffer_usage::constants_buffer | sg::buffer_usage::readonly_buffer
                               | sg::buffer_usage::readwrite_buffer | sg::buffer_usage::indirect_command_buffer;
    auto const probe_info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(size_in_bytes),
        .usage = to_vk_buffer_usage(every_usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer probe = VK_NULL_HANDLE;
    if (VkResult const r = vkCreateBuffer(ctx._device, &probe_info, nullptr, &probe); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateBuffer (memory heap probe) failed");

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(ctx._device, probe, &requirements);
    vkDestroyBuffer(ctx._device, probe, nullptr);

    u32 const type = ctx.find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
        return cc::error("no device-local memory type for a memory heap");

    // Buffers placed here take their device address like any other, so the backing allocation needs the flag too.
    auto const alloc_flags = VkMemoryAllocateFlagsInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    auto const alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &alloc_flags,
        .allocationSize = VkDeviceSize(size_in_bytes),
        .memoryTypeIndex = type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (VkResult const r = vkAllocateMemory(ctx._device, &alloc, nullptr, &memory); r != VK_SUCCESS)
        return vulkan_error(r, "vkAllocateMemory (memory heap) failed");

    return vulkan_memory_heap_handle(std::make_shared<vulkan_memory_heap>(ctx, memory, size_in_bytes, contents, type));
}

vulkan_memory_heap::~vulkan_memory_heap()
{
    if (_memory != VK_NULL_HANDLE)
        vkFreeMemory(_ctx._device, _memory, nullptr);
}

sg::memory_requirements vulkan_memory_heap::query_buffer_requirements(isize size_in_bytes, sg::buffer_usages usage) const
{
    if (size_in_bytes == 0)
        return sg::memory_requirements{.alignment_in_bytes = 1, .size_in_bytes = 0};

    auto const info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(size_in_bytes),
        .usage = _ctx.vk_buffer_usage(usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer probe = VK_NULL_HANDLE;
    VkResult const r = vkCreateBuffer(_ctx._device, &info, nullptr, &probe);
    CC_ASSERT(r == VK_SUCCESS, "vkCreateBuffer (requirements probe) failed");

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(_ctx._device, probe, &requirements);
    vkDestroyBuffer(_ctx._device, probe, nullptr);

    return sg::memory_requirements{
        .alignment_in_bytes = _ctx.vk_buffer_alignment(usage, requirements.alignment),
        .size_in_bytes = isize(requirements.size),
    };
}

sg::memory_requirements vulkan_memory_heap::query_texture_requirements(sg::texture_description const& desc) const
{
    auto image = _ctx.create_vk_image(desc);
    if (image.has_error())
        return {.placeable = false};
    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(_ctx._device, image.value(), &requirements);
    vkDestroyImage(_ctx._device, image.value(), nullptr);

    return sg::memory_requirements{
        .alignment_in_bytes = isize(requirements.alignment),
        .size_in_bytes = isize(requirements.size),
        .placeable = _memory_type != UINT32_MAX && (requirements.memoryTypeBits & (1u << _memory_type)) != 0,
    };
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
cc::result<vulkan_memory_heap_handle> vulkan_context::create_vulkan_memory_heap(isize size_in_bytes,
                                                                                sg::memory_heap_contents contents)
{
    return vulkan_memory_heap::create(*this, size_in_bytes, contents);
}
} // namespace sg::backend::vulkan
