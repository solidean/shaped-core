#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/query/driver_statistic.hh>

namespace sg::backend::vulkan
{
/// What the driver reports of `pipeline`'s executables; empty unless the context captures driver statistics.
[[nodiscard]] cc::vector<sg::driver_statistic> driver_statistics_of(vulkan_context const& ctx, VkPipeline pipeline);
} // namespace sg::backend::vulkan
