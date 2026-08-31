#pragma once

// Single include gate for the Vulkan headers plus the shared error helper.
// vulkan TUs include this, not <vulkan/vulkan.h> directly.

#include <clean-core/error/result.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/fwd.hh> // also what puts the bare sized aliases in scope inside sg

// Under VK_USE_PLATFORM_WIN32_KHR — the surface TUs on Windows — vulkan_win32.h reaches windows.h, and an unsanitized
// one costs us the `byte` typedef and the min()/max() macros alike, so it goes through clean-core's gate first.
// See win32_sanitized.hh for why the `byte` rename is needed; the bracket is repeated here the way every other gate repeats it.
// The clean-core includes stay above the bracket: a C++ header parsed under the macro would lose `std::byte`.
#ifdef VK_USE_PLATFORM_WIN32_KHR
#include <clean-core/platform/win32_sanitized.hh>

#define byte win_byte_override
#include <vulkan/vulkan.h>
#undef byte
#else
#include <vulkan/vulkan.h>
#endif

namespace sg::backend::vulkan
{
/// Name for a VkResult: the core codes up to the 1.2 baseline.
/// Newer or extension codes fall back to "VK_RESULT_<unknown>", and vulkan_error prints the numeric value alongside either way.
[[nodiscard]] char const* vk_result_name(VkResult r);

/// Builds a cc::result error from a failed VkResult, recording the call site (not this helper).
[[nodiscard]] inline auto vulkan_error(VkResult r,
                                       char const* what,
                                       cc::source_location site = cc::source_location::current())
{
    return cc::error(cc::format("{} ({} = {})", what, vk_result_name(r), i32(r)), site);
}
} // namespace sg::backend::vulkan
