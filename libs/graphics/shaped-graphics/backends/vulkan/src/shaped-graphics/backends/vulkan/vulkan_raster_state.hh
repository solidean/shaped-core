#pragma once

#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raster/blend_state.hh>
#include <shaped-graphics/raster/depth_stencil_state.hh>
#include <shaped-graphics/raster/primitive_topology.hh>
#include <shaped-graphics/raster/rasterization_state.hh>
#include <shaped-graphics/raster/vertex_input.hh>

/// sg's fixed-function raster vocabulary translated into Vulkan.
///
/// Every enumerator sg declares carries its Vulkan spelling in a trailing comment, the same way the access vocabulary
/// does, so this is a mapping function rather than a design.
/// Kept device-free, so its tests run on a machine with no Vulkan device.

namespace sg::backend::vulkan
{
[[nodiscard]] VkPrimitiveTopology to_vk_topology(sg::primitive_topology t);
[[nodiscard]] VkPolygonMode to_vk_polygon_mode(sg::fill_mode m);
[[nodiscard]] VkCullModeFlags to_vk_cull_mode(sg::cull_mode m);
[[nodiscard]] VkFrontFace to_vk_front_face(sg::front_face f);
[[nodiscard]] VkBlendFactor to_vk_blend_factor(sg::blend_factor f);
[[nodiscard]] VkBlendOp to_vk_blend_op(sg::blend_op op);
[[nodiscard]] VkColorComponentFlags to_vk_color_write_mask(sg::color_write_mask mask);
[[nodiscard]] VkStencilOp to_vk_stencil_op(sg::stencil_op op);
[[nodiscard]] VkFormat to_vk_vertex_format(sg::vertex_attribute_format f);
} // namespace sg::backend::vulkan
