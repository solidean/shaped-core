#pragma once

#include <clean-core/fwd.hh>

#include <memory>

/// Forward declarations for the DirectX 12 backend.

namespace sg::backend::dx12
{
/// Monotonic value on the async-upload completion fence (dx12_upload_async_system::_completion_fence).
/// Its own newtype so it can't be confused with the epoch / submission / staging fence timelines: a
/// later direct-queue list waits on this value to see an async upload's writes. `none` == no pending
/// upload.
enum class dx12_copy_fence_value : cc::u64
{
    none = 0,
};

/// Monotonic value on the async-download completion fence (dx12_download_async_system::_completion_fence).
/// Its own newtype so it can't be confused with the other fence timelines: a later direct-queue list that
/// WRITES a buffer waits on this value to know the async readback has finished reading it. `none` == no
/// pending async download.
enum class dx12_download_fence_value : cc::u64
{
    none = 0,
};

class dx12_context;
class dx12_command_list;
class dx12_command_allocator_pool;
class dx12_buffer;
using dx12_buffer_handle = std::shared_ptr<dx12_buffer const>;
class dx12_texture;
using dx12_texture_handle = std::shared_ptr<dx12_texture const>;
class dx12_memory_heap;
using dx12_memory_heap_handle = std::shared_ptr<dx12_memory_heap const>;

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
class dx12_binding_group;
using dx12_binding_group_layout_handle = std::shared_ptr<dx12_binding_group_layout const>;
using dx12_pipeline_layout_handle = std::shared_ptr<dx12_pipeline_layout const>;
using dx12_compute_pipeline_handle = std::shared_ptr<dx12_compute_pipeline const>;
using dx12_binding_group_handle = std::shared_ptr<dx12_binding_group const>;

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
