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
struct vulkan_async_upload_job;      // one async upload handed to the copy actor
struct vulkan_transfer_wake;         // a bare wake for the copy actor
class vulkan_upload_actor;           // drains async uploads in enqueue order
class vulkan_upload_waker;           // the wake channel handed to every stream source
class vulkan_upload_async_system;    // ctx.upload's transfer-queue system
struct vulkan_async_download_job;    // one async readback handed to the readback actor
class vulkan_download_async_actor;   // drains them in enqueue order
class vulkan_download_async_system;  // ctx.download's transfer-queue system
struct vulkan_download_copy_job;     // one staged readback awaiting its copy-out (see vulkan_download_inline.hh)
class vulkan_download_actor;         // drains them in submission order
class vulkan_download_inline_system; // the readback ring
struct vulkan_query_pool_lease;      // one VkQueryPool leased by a command list
class vulkan_query_system;           // the pool of them
struct vulkan_completion_group;      // one resource's transfer timeline in one direction
using vulkan_completion_group_handle = std::shared_ptr<vulkan_completion_group>;
struct vulkan_group_value;          // a completion value plus the timeline it belongs to
class vulkan_completion_group_pool; // hands them out and takes them back
struct vulkan_descriptor_functions; // the VK_EXT_descriptor_buffer entry points, loaded per device
struct vulkan_raytracing_functions; // the acceleration-structure + ray-tracing-pipeline entry points
class vulkan_blas;
class vulkan_tlas;
struct vulkan_descriptor_range;    // one allocated range within the descriptor heap
class vulkan_descriptor_heap;      // host-visible memory descriptors are written into
struct vulkan_image_view_key;      // the identity one cached VkImageView is keyed on
class vulkan_image_view_cache;     // VkImageViews for bound texture views, keyed by view identity
class vulkan_sampler_cache;        // VkSamplers for bound sampler states, keyed by sampler identity
struct vulkan_hazard_view;         // a bound buffer + the access class it is used as (see vulkan_binding_group.hh)
struct vulkan_texture_hazard_view; // the texture analogue
struct vulkan_array_element;       // one element of an array binding
struct vulkan_array_binding;       // an array binding's per-element resources
class vulkan_binding_group;
using vulkan_binding_group_handle = std::shared_ptr<vulkan_binding_group const>;
class vulkan_staging_binding_group;
using vulkan_staging_binding_group_handle = std::shared_ptr<vulkan_staging_binding_group>;
class vulkan_binding_group_layout;
using vulkan_binding_group_layout_handle = std::shared_ptr<vulkan_binding_group_layout const>;
struct vulkan_array_buffer_declare;  // one declare_array_buffer_access, held until the next dispatch
struct vulkan_array_texture_declare; // the texture analogue
class vulkan_compute_pipeline;
class vulkan_raster_pipeline;
class vulkan_swapchain;
using vulkan_swapchain_handle = std::shared_ptr<vulkan_swapchain>;
class vulkan_raytracing_pipeline;
using vulkan_raytracing_pipeline_handle = std::shared_ptr<vulkan_raytracing_pipeline const>;
class vulkan_raytracing_shader_table;
using vulkan_raytracing_shader_table_handle = std::shared_ptr<vulkan_raytracing_shader_table const>;
using vulkan_raster_pipeline_handle = std::shared_ptr<vulkan_raster_pipeline const>;
using vulkan_compute_pipeline_handle = std::shared_ptr<vulkan_compute_pipeline const>;
class vulkan_pipeline_layout;
using vulkan_pipeline_layout_handle = std::shared_ptr<vulkan_pipeline_layout const>;
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
