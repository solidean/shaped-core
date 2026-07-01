#pragma once

/// Forward declarations for the DirectX 12 backend. Types are smurf-named (dx12_*) and live in
/// sg::backend::dx12 so backend symbols stay greppable and never collide across backends.

namespace sg::backend::dx12
{
class dx12_context;
class dx12_command_list;
class dx12_buffer;
} // namespace sg::backend::dx12
