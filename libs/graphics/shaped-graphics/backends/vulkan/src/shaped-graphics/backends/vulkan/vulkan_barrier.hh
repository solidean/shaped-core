#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/barrier/resource_access.hh>
#include <shaped-graphics/barrier/resource_access_state.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/subresource.hh>

/// vulkan owns its barrier emission entirely — the sg core only hands it the `access_barrier` computed by the
/// shared `resource_access_state` machine, and this file turns one of those into Vulkan's synchronization2 structs.
///
/// The sg vocabulary was written against `PIPELINE_STAGE_2` / `ACCESS_2` in the first place, so every mapping below
/// is one-to-one and the interesting work is elsewhere: what to declare, and when to flush.
/// See libs/graphics/shaped-graphics/docs/concepts/barriers.md.

namespace sg::backend::vulkan
{
/// The stage mask an sg stage set implies; an empty set is `NONE`, which is what a layout-only transition wants.
[[nodiscard]] VkPipelineStageFlags2 vk_stage2_from(sg::pipeline_stage_flags stages);

/// The access mask an sg access set implies; an empty set is `NONE`.
[[nodiscard]] VkAccessFlags2 vk_access2_from(sg::access_flags access);

/// The image layout an sg texture layout means.
/// `shader_readwrite` and `general` both map to `VK_IMAGE_LAYOUT_GENERAL` — Vulkan has no separate storage layout.
[[nodiscard]] VkImageLayout vk_layout_from(sg::texture_layout layout);

/// The aspect mask a subresource range's aspect span covers.
[[nodiscard]] VkImageAspectFlags vk_aspect_mask_from(sg::subresource_range const& range);

/// A whole-buffer memory barrier for `barrier`.
/// Vulkan can scope a buffer barrier to a byte range, but sg tracks access per resource rather than per range, so
/// there is nothing narrower to say and the barrier covers the whole buffer.
[[nodiscard]] VkBufferMemoryBarrier2 make_buffer_barrier(VkBuffer buffer, sg::access_barrier const& barrier);

/// An image barrier for `barrier`, scoped to `range`.
/// A `src_layout` of `undefined` means the previous contents are not preserved, which is exactly what Vulkan's
/// `VK_IMAGE_LAYOUT_UNDEFINED` as an old layout already says — so a discard needs no separate flag.
[[nodiscard]] VkImageMemoryBarrier2 make_image_barrier(VkImage image,
                                                       sg::subresource_range const& range,
                                                       sg::access_barrier const& barrier);

/// Records one `vkCmdPipelineBarrier2` for everything staged, or nothing at all when both spans are empty.
void submit_barriers(VkCommandBuffer cmd,
                     cc::span<VkBufferMemoryBarrier2 const> buffer_barriers,
                     cc::span<VkImageMemoryBarrier2 const> image_barriers);
} // namespace sg::backend::vulkan
