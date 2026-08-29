#pragma once

#include <shaped-graphics/fwd.hh> // sg's door to <memory>, which the context's command-list methods need

/// Forward declarations for the Vulkan backend.

namespace sg::backend::vulkan
{
struct vulkan_config; // instance/device creation knobs (see vulkan_context.hh)
class vulkan_context;
enum class vulkan_message_severity;
class vulkan_command_list;
class vulkan_buffer;
class vulkan_texture;

/// Backend-typed resource handles.
/// Mutable, where sg's `*_handle` typedefs and dx12's backend-typed ones are `shared_ptr<T const>` — an inconsistency to settle, not a design.
/// No command-list handle: a list is move-only, held by std::unique_ptr<vulkan_command_list>.
using vulkan_buffer_handle = std::shared_ptr<vulkan_buffer const>;
using vulkan_texture_handle = std::shared_ptr<vulkan_texture const>;

/// The domain every recording site in the Vulkan backend is attributed to.
/// It shadows sg's, so a backend message is never mistaken for a portable one.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace sg::backend::vulkan
