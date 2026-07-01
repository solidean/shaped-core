#pragma once

/// Forward declarations for the Vulkan backend. Types are smurf-named (vulkan_*) and live in
/// sg::backend::vulkan so backend symbols stay greppable and never collide across backends.

namespace sg::backend::vulkan
{
class vulkan_context;
class vulkan_command_list;
} // namespace sg::backend::vulkan
