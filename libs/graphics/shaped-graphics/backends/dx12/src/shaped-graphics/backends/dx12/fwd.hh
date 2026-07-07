#pragma once

#include <memory>

/// Forward declarations for the DirectX 12 backend.

namespace sg::backend::dx12
{
class dx12_context;
class dx12_command_list;
class dx12_command_allocator_pool;
class dx12_buffer;
using dx12_buffer_handle = std::shared_ptr<dx12_buffer const>;
class dx12_texture;
using dx12_texture_handle = std::shared_ptr<dx12_texture const>;
class dx12_memory_heap;
using dx12_memory_heap_handle = std::shared_ptr<dx12_memory_heap const>;

// Bind path (see dx12_binding_layout.hh / dx12_compute_pipeline.hh / dx12_binding_group.hh).
struct dx12_descriptor_heap;
class dx12_binding_layout;
class dx12_compute_pipeline;
class dx12_binding_group;
using dx12_binding_layout_handle = std::shared_ptr<dx12_binding_layout const>;
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
} // namespace sg::backend::dx12
