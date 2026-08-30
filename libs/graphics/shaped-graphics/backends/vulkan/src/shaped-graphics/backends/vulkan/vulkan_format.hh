#pragma once

#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture_descriptions.hh>
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
} // namespace sg::backend::vulkan
