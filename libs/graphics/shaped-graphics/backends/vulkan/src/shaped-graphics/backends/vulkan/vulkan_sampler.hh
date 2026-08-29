#pragma once

#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/fwd.hh>

/// sg's sampler value type translated into Vulkan.
///
/// Unlike D3D12, Vulkan keeps the per-axis min/mag/mip filters independent of anisotropy rather than folding
/// anisotropy into an encoded filter, so the mapping is field for field.

namespace sg::backend::vulkan
{
/// The VkSamplerCreateInfo an sg sampler describes.
/// A comparison sampler sets compareEnable.
/// The others leave compareOp at NEVER, which Vulkan ignores.
[[nodiscard]] VkSamplerCreateInfo to_vk_sampler_info(sg::sampler const& s);

/// The Vulkan comparison function for an sg compare_op.
/// Shared with depth-stencil state.
[[nodiscard]] VkCompareOp to_vk_compare_op(sg::compare_op op);
} // namespace sg::backend::vulkan
