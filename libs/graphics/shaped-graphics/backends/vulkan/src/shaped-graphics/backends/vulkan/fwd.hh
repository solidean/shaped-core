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
struct vulkan_buffer_access;         // cross-list access tracking for one buffer (see vulkan_buffer_access.hh)
struct vulkan_upload_allocation;     // one reservation in the inline upload ring (see vulkan_upload_inline.hh)
class vulkan_upload_inline_system;   // the ring itself
struct vulkan_download_copy_job;     // one staged readback awaiting its copy-out (see vulkan_download_inline.hh)
class vulkan_download_actor;         // drains them in submission order
class vulkan_download_inline_system; // the readback ring
class vulkan_memory_heap;
using vulkan_memory_heap_handle = std::shared_ptr<vulkan_memory_heap const>;
class vulkan_texture;
struct vulkan_subresource_barrier; // one barrier scoped to a subresource range (see vulkan_texture_access.hh)
enum class layout_combine;         // how two required layouts for one op combined
struct combined_layout;
class vulkan_texture_access; // per-texture, per-command-list layout tracking

/// Backend-typed resource handles.
/// Mutable, where sg's `*_handle` typedefs and dx12's backend-typed ones are `shared_ptr<T const>` — an inconsistency to settle, not a design.
/// No command-list handle: a list is move-only, held by std::unique_ptr<vulkan_command_list>.
using vulkan_buffer_handle = std::shared_ptr<vulkan_buffer const>;
using vulkan_texture_handle = std::shared_ptr<vulkan_texture const>;

/// The domain every recording site in the Vulkan backend is attributed to.
/// It shadows sg's, so a backend message is never mistaken for a portable one.
CC_REC_DECLARE_DOMAIN(g_rec_domain);
} // namespace sg::backend::vulkan
