#pragma once

#include <shaped-graphics/fwd.hh>

/// Small vocabulary enums shared across the shaped-graphics public API.

namespace sg
{
/// Coarse tag for the kind of backend behind a context — mainly to interpret raw handles from
/// escape hatches. Not the concrete type, and not exhaustive (debug/cpu/remote backends may exist),
/// so don't switch on it as a closed set. More kinds will be added.
enum class backend_kind
{
    dx12,
    vulkan,
    metal,
    webgpu,
    opengl,
    webgl,
};

/// The threading guarantees a backend's context provides. Coarse for now; expected to gain nuance
/// (e.g. whether concurrent command-list recording is allowed). See
/// libs/graphics/shaped-graphics/docs/concepts/threading.md.
enum class thread_model
{
    single_threaded, ///< every context operation must be externally synchronized to one thread at a time
    multi_threaded,  ///< resource / command-list ops (create / submit / drop) are safe to call concurrently;
                     ///< epoch management (advance, waits) and shutdown must be externally synchronized
};

/// How a buffer's storage may be used. Bit flags — combine with `|`, test with `has_flag`.
/// Migrates to `cc::flags` once that clean-core type lands.
enum class buffer_usage : u32
{
    none = 0,
    copy_src = 1u << 0,
    copy_dst = 1u << 1,
    vertex = 1u << 2,
    index = 1u << 3,
    uniform = 1u << 4,
    storage = 1u << 5,
};

[[nodiscard]] constexpr buffer_usage operator|(buffer_usage a, buffer_usage b)
{
    return buffer_usage(u32(a) | u32(b));
}

[[nodiscard]] constexpr buffer_usage operator&(buffer_usage a, buffer_usage b)
{
    return buffer_usage(u32(a) & u32(b));
}

/// True if every bit in `flag` is set in `usage`.
[[nodiscard]] constexpr bool has_flag(buffer_usage usage, buffer_usage flag)
{
    return (u32(usage) & u32(flag)) == u32(flag);
}
} // namespace sg
