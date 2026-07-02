#pragma once

#include <memory>

/// Forward declarations for the Vulkan backend. Types are smurf-named (vulkan_*) and live in
/// sg::backend::vulkan so backend symbols stay greppable and never collide across backends.

namespace sg::backend::vulkan
{
class vulkan_context;
class vulkan_command_list;
class vulkan_buffer;

/// Backend-typed buffer handle (shared, like buffer_handle). No command-list handle — those are
/// move-only, held by std::unique_ptr<vulkan_command_list>.
using vulkan_buffer_handle = std::shared_ptr<vulkan_buffer>;
} // namespace sg::backend::vulkan
