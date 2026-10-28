#pragma once

#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/memory/memory_heap.hh>

/// One device-local VkDeviceMemory that buffers or textures are placed into at caller-chosen offsets.
///
/// The heap mints placements and answers requirement queries; it does not track what is placed where.
/// That belongs to whatever allocator the caller runs on top — ctx.transient's bump allocator is the one in-tree
/// consumer today.
///
/// A textures heap takes its memory type from a probe image, since an image's memoryTypeBits need not intersect a buffer's.
/// An image whose own types exclude it is reported unplaceable rather than bound where it may not live.
class sg::backend::vulkan::vulkan_memory_heap final : public sg::memory_heap
{
public:
    /// Allocates a device-local heap of `size_in_bytes`, which must be >= 0.
    /// Size 0 is a legal empty heap holding no placements, with a null VkDeviceMemory — the same convention an empty
    /// buffer follows.
    [[nodiscard]] static cc::result<vulkan_memory_heap_handle> create(vulkan_context& ctx,
                                                                      isize size_in_bytes,
                                                                      sg::memory_heap_contents contents);

    vulkan_memory_heap(vulkan_context& ctx,
                       VkDeviceMemory memory,
                       isize size_in_bytes,
                       sg::memory_heap_contents contents,
                       u32 memory_type)
      : sg::memory_heap(size_in_bytes, contents), _ctx(ctx), _memory(memory), _memory_type(memory_type)
    {
    }

    /// Frees the heap's memory immediately rather than deferring it.
    /// Every buffer placed in it holds a handle to it, so the heap outlives its placements by construction.
    ~vulkan_memory_heap() override;

    vulkan_context& _ctx;
    VkDeviceMemory _memory = VK_NULL_HANDLE;
    /// The memory type `_memory` was allocated from; UINT32_MAX for an empty heap.
    u32 _memory_type = UINT32_MAX;

protected:
    /// The alignment a placed buffer's offset must satisfy and the size it occupies, from vkGetBufferMemoryRequirements
    /// on a throwaway buffer of the same shape.
    /// Creating one to ask is what Vulkan offers: unlike D3D12 there is no query that takes a description directly.
    [[nodiscard]] sg::memory_requirements query_buffer_requirements(isize size_in_bytes,
                                                                    sg::buffer_usages usage) const override;
    /// The same for a texture, from vkGetImageMemoryRequirements on a throwaway image of the description.
    [[nodiscard]] sg::memory_requirements query_texture_requirements(sg::texture_description const& desc) const override;
};
