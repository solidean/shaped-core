#pragma once

#include <memory>

/// Forward declarations for the DirectX 12 backend.

namespace sg::backend::dx12
{
class dx12_context;
class dx12_command_list;
class dx12_command_allocator_pool;
class dx12_buffer;

// Inline buffer transfer (see dx12_upload_inline.hh / dx12_download_inline.hh and the resource helpers).
class dx12_upload_inline_system;
class dx12_download_inline_system;
struct dx12_resource_upload;
struct dx12_buffer_upload;
struct dx12_resource_download;
struct dx12_buffer_download;
class dx12_download_waiter;
struct dx12_download_copy_job;

/// Backend-typed buffer handle (shared, like buffer_handle). No command-list handle — those are
/// move-only, held by std::unique_ptr<dx12_command_list>.
using dx12_buffer_handle = std::shared_ptr<dx12_buffer>;
} // namespace sg::backend::dx12
