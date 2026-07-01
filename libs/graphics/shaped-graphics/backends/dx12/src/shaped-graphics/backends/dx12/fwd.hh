#pragma once

#include <memory>

/// Forward declarations for the DirectX 12 backend. Types are smurf-named (dx12_*) and live in
/// sg::backend::dx12 so backend symbols stay greppable and never collide across backends.

namespace sg::backend::dx12
{
class dx12_context;
class dx12_command_list;
class dx12_buffer;

/// Backend-typed handle for the shared-immutable buffer resource. Backends define their own handles
/// so callers that stay on one backend hold the concrete type and avoid downcasts. There is
/// deliberately no dx12_command_list_handle: command lists are move-only temporaries held by
/// std::unique_ptr<dx12_command_list> directly (see the sg coding guidelines).
using dx12_buffer_handle = std::shared_ptr<dx12_buffer>;
} // namespace sg::backend::dx12
