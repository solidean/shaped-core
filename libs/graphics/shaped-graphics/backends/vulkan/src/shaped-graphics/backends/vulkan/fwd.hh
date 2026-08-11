#pragma once

#include <memory>

/// Forward declarations for the Vulkan backend.

namespace sg::backend::vulkan
{
struct vulkan_config; // instance/device creation knobs (see vulkan_context.hh)
class vulkan_context;
class vulkan_command_list;
class vulkan_buffer;
class vulkan_texture;

/// Backend-typed resource handles.
/// Mutable, where sg's `*_handle` typedefs and dx12's backend-typed ones are `shared_ptr<T const>` — an inconsistency to settle, not a design.
/// No command-list handle: a list is move-only, held by std::unique_ptr<vulkan_command_list>.
using vulkan_buffer_handle = std::shared_ptr<vulkan_buffer>;
using vulkan_texture_handle = std::shared_ptr<vulkan_texture>;
} // namespace sg::backend::vulkan
