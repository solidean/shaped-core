#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture_descriptions.hh>
#include <shaped-graphics/resource/texture_region.hh>
#include <shaped-graphics/types.hh>

/// sg vocabulary translated into Vulkan enums and flag bits.
/// These live here rather than beside one resource type because several unrelated TUs need the same mapping —
/// a swapchain picks a surface format, a view builds an image view, a pipeline declares its attachment formats,
/// and a texture copy sizes its staging footprint.

namespace sg::backend::vulkan
{
/// The VkFormat for an sg pixel_format; asserts on one it does not map.
[[nodiscard]] VkFormat to_vk_format(sg::pixel_format f);

/// The VkImageType for a texture dimension.
[[nodiscard]] VkImageType to_vk_image_type(sg::texture_dimension d);

/// The VkImageUsageFlags an sg texture usage set implies.
/// Falls back to SAMPLED_BIT for an empty set, so a usage-less texture is still a legal image.
[[nodiscard]] VkImageUsageFlags to_vk_image_usage(sg::texture_usages u);

/// The VkBufferUsageFlags an sg buffer usage set implies.
/// Falls back to TRANSFER_DST_BIT for an empty set, so a usage-less buffer is still legal.
[[nodiscard]] VkBufferUsageFlags to_vk_buffer_usage(sg::buffer_usages usage);

/// Tightly-packed bytes per row of `region` — a row of BLOCKS, so one BC row covers four texel rows.
/// This is the granularity a streamed texture's chunks and a transfer window are both clamped to.
[[nodiscard]] isize region_row_bytes(sg::pixel_format format, sg::texture_region const& region);

/// How many block rows one depth slice of `region` holds.
[[nodiscard]] isize region_block_rows(sg::pixel_format format, sg::texture_region const& region);

/// The copies that move `row_count` block rows of `region`, starting at block row `first_row`, between an image and a
/// tightly-packed buffer range starting at `buffer_offset`.
///
/// Rows are counted slice-major: a 3D region's rows run through a whole depth slice before the next, so a band can
/// cross slices, and that takes one copy per slice it touches.
/// Each copy's extent is in texels, clamped to the region, which is what lets its last block row be partial.
void append_block_row_copies(cc::vector<VkBufferImageCopy>& out,
                             sg::pixel_format format,
                             VkImageSubresourceLayers const& subresource,
                             sg::texture_region const& region,
                             isize first_row,
                             isize row_count,
                             VkDeviceSize buffer_offset);
} // namespace sg::backend::vulkan
