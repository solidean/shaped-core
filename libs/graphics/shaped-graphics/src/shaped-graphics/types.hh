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

/// How a buffer may be used across the pipeline. Bit flags — combine with `|`, test with `has_flag`.
/// Migrates to `cc::flags` once that clean-core type lands.
///
/// Fine-grained on purpose: a backend may need every usage declared at buffer creation, and a distinction
/// merged here can't be recovered downstream. Read-vs-write matters only where creation needs it; a pure
/// write-only buffer isn't representable — that's a shader/binding access mode, not a creation usage, and
/// collapses into `storage_read_write`.
enum class buffer_usage : u32
{
    none = 0,
    copy_src = 1u << 0,           // Vk TRANSFER_SRC / WGPU COPY_SRC; DX12 & Metal implicit
    copy_dst = 1u << 1,           // Vk TRANSFER_DST / WGPU COPY_DST; DX12 initial state; Metal implicit
    vertex = 1u << 2,             // Vk VERTEX / WGPU VERTEX
    index = 1u << 3,              // Vk INDEX / WGPU INDEX
    uniform = 1u << 4,            // constant buffer: Vk UNIFORM(_TEXEL) / WGPU UNIFORM (size-capped)
    storage_read = 1u << 5,       // read-only structured/raw SRV: Vk STORAGE(_TEXEL) / WGPU STORAGE; DX12 no flag
    storage_read_write = 1u << 6, // UAV: Vk STORAGE / WGPU STORAGE; DX12 ALLOW_UNORDERED_ACCESS
    indirect = 1u << 7,           // Vk INDIRECT / WGPU INDIRECT

    // Acceleration-structure (raytracing). Vulkan also needs a buffer device address for these; the
    // backend adds it implicitly (no separate `device_address` usage for now).
    accel_struct_storage = 1u << 8,     // Vk AS_STORAGE_KHR; DX12 AS resource state
    accel_struct_build_input = 1u << 9, // Vk AS_BUILD_INPUT_READ_ONLY_KHR; DX12 plain SRV

    // Not yet modeled — add when a backend needs them:
    // conditional_rendering  — Vk CONDITIONAL_RENDERING_EXT only (DX12 predication implicit; no WGPU/Metal)
    // stream_output          — transform feedback: Vk XFB EXT / DX12 stream-output
    // shared/external        — NOT a usage: cross-device sharing is a memory property; belongs on
    //                          memory_heap / allocation_info (DX12 HEAP_FLAG_SHARED / Vk external memory)
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
