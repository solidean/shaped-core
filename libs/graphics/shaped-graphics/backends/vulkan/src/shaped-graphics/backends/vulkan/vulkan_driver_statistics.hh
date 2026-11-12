#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/query/driver_statistic.hh>

namespace sg::backend::vulkan
{
/// What the driver reports of `pipeline`'s executables; empty unless the context captures driver statistics.
[[nodiscard]] cc::vector<sg::driver_statistic> driver_statistics_of(vulkan_context const& ctx, VkPipeline pipeline);

/// The forms the driver kept of `pipeline`'s code; empty unless the context captures driver representations.
[[nodiscard]] cc::vector<sg::driver_representation> driver_representations_of(vulkan_context const& ctx,
                                                                              VkPipeline pipeline);
} // namespace sg::backend::vulkan
