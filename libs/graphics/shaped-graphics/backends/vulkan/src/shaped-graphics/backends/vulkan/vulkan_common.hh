#pragma once

// Single include gate for the Vulkan headers plus the shared error helper. vulkan TUs include this,
// not <vulkan/vulkan.h> directly, so the SDK surface enters through one place.

#include <clean-core/error/result.hh>
#include <clean-core/string/format.hh>
#include <vulkan/vulkan.h>

namespace sg::backend::vulkan
{
/// Name for a VkResult: the core codes up to the 1.2 baseline we require. Newer/extension codes fall
/// back to "VK_RESULT_<unknown>" — vulkan_error always prints the numeric value alongside, so nothing
/// is lost either way.
[[nodiscard]] char const* vk_result_name(VkResult r);

/// Builds a cc::result error from a failed VkResult, recording the call site (not this helper). Prints
/// both the name and the numeric code, so an unnamed (extension/newer) result is still identifiable.
[[nodiscard]] inline auto vulkan_error(VkResult r,
                                       char const* what,
                                       cc::source_location site = cc::source_location::current())
{
    return cc::error(cc::format("{} ({} = {})", what, vk_result_name(r), cc::i32(r)), site);
}
} // namespace sg::backend::vulkan
