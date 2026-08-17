#pragma once

#include <shaped-graphics/fwd.hh> // also what puts the bare sized aliases in scope inside sg, and sg's door to <memory>

/// Forward declarations for the DirectX 12 backend.

namespace sg::backend::dx12
{
// Declared here so each header defines its types qualified rather than opening the namespace around them.
struct dx12_config;
struct dx12_descriptor_alloc;
struct dx12_pooled_allocator;
struct dx12_acquired_command_list;
struct dx12_hazard_view;
struct dx12_texture_hazard_view;
struct dx12_upload_allocation;
struct dx12_download_allocation;
struct dx12_pending_copy;
struct dx12_texture_upload;
struct dx12_texture_download;

enum class dx12_message_severity : u8; // how bad a debug-layer message is (see dx12_context.hh)

enum class cpu_descriptor_slot : int;  // a slot in a CPU descriptor heap (see dx12_cpu_descriptor_heap.hh)
enum class dx12_query_heap_type : u32; // which query heap a query lives in (see dx12_query.hh)
enum class layout_combine;             // how two texture layouts merge (see dx12_texture_access.hh)

// The two fence-value newtypes this header goes on to DEFINE, named here so each definition can be written qualified.
enum class dx12_copy_fence_value : u64;
enum class dx12_download_fence_value : u64;

} // namespace sg::backend::dx12

/// Monotonic value on the async-upload completion fence (dx12_upload_async_system::_completion_fence).
/// Its own newtype so it cannot be confused with the epoch / submission / staging fence timelines.
/// A later direct-queue list waits on this value to see an async upload's writes; `none` means no pending upload.
enum class sg::backend::dx12::dx12_copy_fence_value : sg::u64
{
    none = 0,
};

/// Monotonic value on the async-download completion fence (dx12_download_async_system::_completion_fence).
/// Its own newtype so it cannot be confused with the other fence timelines.
/// A later direct-queue list that WRITES a buffer waits on this value to know the async readback has finished reading it; `none` means no pending async download.
enum class sg::backend::dx12::dx12_download_fence_value : sg::u64
{
    none = 0,
};

namespace sg::backend::dx12
{

class dx12_context;
/// A backend-typed context handle: an sg::context_handle known to point at a dx12_context.
/// create_dx12_context hands back the abstract sg::context_handle, since that is what a caller drives; this is for code already committed to dx12 — the backend's own tests above all.
using dx12_context_handle = std::shared_ptr<dx12_context>;

class dx12_command_list;
class dx12_command_allocator_pool;
class dx12_buffer;
using dx12_buffer_handle = std::shared_ptr<dx12_buffer const>;
class dx12_texture;
using dx12_texture_handle = std::shared_ptr<dx12_texture const>;
class dx12_memory_heap;
using dx12_memory_heap_handle = std::shared_ptr<dx12_memory_heap const>;
class dx12_swapchain;
using dx12_swapchain_handle = std::shared_ptr<dx12_swapchain>; // mutable: a per-frame present driver

// Ray-tracing acceleration structures (see dx12_acceleration_structure.hh).
class dx12_blas;
using dx12_blas_handle = std::shared_ptr<dx12_blas const>;
class dx12_tlas;
using dx12_tlas_handle = std::shared_ptr<dx12_tlas const>;

// Bind path (see dx12_binding_group_layout.hh / dx12_pipeline_layout.hh / dx12_compute_pipeline.hh /
// dx12_binding_group.hh).
struct dx12_descriptor_heap;
class dx12_binding_group_layout;
class dx12_pipeline_layout;
class dx12_compute_pipeline;
class dx12_raster_pipeline;
class dx12_binding_group;
using dx12_binding_group_layout_handle = std::shared_ptr<dx12_binding_group_layout const>;
using dx12_pipeline_layout_handle = std::shared_ptr<dx12_pipeline_layout const>;
using dx12_compute_pipeline_handle = std::shared_ptr<dx12_compute_pipeline const>;
using dx12_raster_pipeline_handle = std::shared_ptr<dx12_raster_pipeline const>;
using dx12_binding_group_handle = std::shared_ptr<dx12_binding_group const>;

// Ray-tracing pipeline + shader table (see dx12_raytracing_pipeline.hh / dx12_raytracing_shader_table.hh).
class dx12_raytracing_pipeline;
using dx12_raytracing_pipeline_handle = std::shared_ptr<dx12_raytracing_pipeline const>;
class dx12_raytracing_shader_table;
using dx12_raytracing_shader_table_handle = std::shared_ptr<dx12_raytracing_shader_table const>;

// Inline buffer transfer (see dx12_upload_inline.hh / dx12_download_inline.hh and the resource helpers).
class dx12_upload_inline_system;
class dx12_download_inline_system;
struct dx12_resource_upload;
struct dx12_buffer_upload;
struct dx12_resource_download;
struct dx12_buffer_download;
class dx12_download_waiter;
struct dx12_download_copy_job;

// Async buffer upload on a dedicated copy queue (see dx12_upload_async.hh).
class dx12_upload_async_system;
struct dx12_async_upload_job;

// Async buffer download on a dedicated copy queue (see dx12_download_async.hh).
class dx12_download_async_system;
struct dx12_async_download_job;
class dx12_async_download_waiter;
} // namespace sg::backend::dx12
