#pragma once

#include <shaped-graphics/fwd.hh>

/// Small vocabulary enums shared across the shaped-graphics public API.

namespace sg
{
/// A coarse, informational tag for the kind of backend behind a context — used mainly to
/// interpret raw underlying handles obtained through escape hatches. It is **not** the concrete
/// backend type and **not** exhaustive: sg does not assume it knows every backend (a debug, cpu,
/// or remote implementation may exist), so don't switch on it as if it were a closed set. The
/// listed kinds are the currently-planned ones; more will be added.
enum class backend_kind
{
    dx12,
    vulkan,
    metal,
    webgpu,
    opengl,
    webgl,
};

/// How a buffer's GPU-resident storage may be used. Bit flags — combine with `|`, test with
/// `has_flag`. Migrates to `cc::flags` once that clean-core type lands.
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
