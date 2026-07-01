#pragma once

/// Forward declarations for the sg-level backend bridge (the abstract interfaces the public
/// types delegate to). The concrete, smurf-named implementations live in the per-backend
/// static libraries under backends/ (e.g. sg::backend::dx12::dx12_context).

namespace sg
{
class backend_context;
class backend_command_list;
class backend_buffer;
} // namespace sg
