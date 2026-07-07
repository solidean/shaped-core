#pragma once

#include <shaped-graphics/fwd.hh>

/// Backend-neutral vocabulary for resource access tracking: how a GPU operation touches a resource
/// (`access_flags`), in which pipeline stages (`pipeline_stage_flags`), and — for textures — the memory
/// layout it needs (`texture_layout`). Deliberately not any single backend's spelling; each value's
/// trailing comment gives its D3D12 and Vulkan mapping.
///
/// These are shared, opt-in building blocks: a backend that emits explicit barriers tracks state in
/// terms of them (see resource_access_state.hh); a backend that relies on driver-managed barriers may
/// ignore them entirely. Buffers only ever use the `general` layout — layouts matter for textures.

namespace sg
{
/// What a GPU operation does to a resource. Bit flags — combine with `|`, test with `has_all` / `has_any`.
/// Migrates to `cc::flags` once that clean-core type lands (same status as `buffer_usage`).
///
/// Read vs write is explicit in the suffix. `is_unordered_write` marks the accesses that create a hazard
/// needing a barrier (shader/transfer/accel writes); color/depth *target* writes are ROP-ordered by the
/// hardware and are not unordered.
enum class access_flags : u32
{
    none = 0,
    uniform_read = 1u << 0,   // constant-buffer read:   DX12 CONSTANT_BUFFER / Vk UNIFORM_READ
    index_read = 1u << 1,     // index-buffer fetch:      DX12 INDEX_BUFFER    / Vk INDEX_READ
    vertex_read = 1u << 2,    // vertex-buffer fetch:     DX12 VERTEX_BUFFER   / Vk VERTEX_ATTRIBUTE_READ
    shader_read = 1u << 3,    // SRV / sampled+storage:   DX12 SHADER_RESOURCE / Vk SHADER_READ
    shader_write = 1u << 4,   // UAV / storage write:     DX12 UNORDERED_ACCESS/ Vk SHADER_WRITE
    transfer_read = 1u << 5,  // copy/resolve source:     DX12 COPY_SOURCE     / Vk TRANSFER_READ
    transfer_write = 1u << 6, // copy/resolve dest:       DX12 COPY_DEST       / Vk TRANSFER_WRITE
    indirect_read = 1u << 7,  // indirect args:           DX12 INDIRECT_ARGUMENT / Vk INDIRECT_COMMAND_READ

    // Designed-in for later resource families (textures / render targets / raytracing); unused for buffers.
    color_write = 1u << 8,  // render-target write:     DX12 RENDER_TARGET   / Vk COLOR_ATTACHMENT_WRITE
    depth_read = 1u << 9,   // depth/stencil test:      DX12 DEPTH_STENCIL_READ  / Vk DEPTH_STENCIL_ATTACHMENT_READ
    depth_write = 1u << 10, // depth/stencil write:    DX12 DEPTH_STENCIL_WRITE / Vk DEPTH_STENCIL_ATTACHMENT_WRITE
    accel_read = 1u << 11,  // AS read/trace:          DX12 / Vk ACCELERATION_STRUCTURE_READ
    accel_write = 1u << 12, // AS build:               DX12 / Vk ACCELERATION_STRUCTURE_WRITE
};

/// Pipeline stages that may perform an access. Bit flags. Coarse on purpose (tessellation/geometry fold
/// into `vertex`, early/late depth into `attachment`), mirroring how DX12 `BARRIER_SYNC` and Vulkan
/// `PIPELINE_STAGE_2` are typically consumed.
enum class pipeline_stage_flags : u32
{
    none = 0,
    draw_indirect = 1u << 0, // DX12 EXECUTE_INDIRECT / Vk DRAW_INDIRECT
    vertex = 1u << 1,        // vertex-processing stages: DX12 VERTEX_SHADING / Vk VERTEX_SHADER (+ pre-raster)
    fragment = 1u << 2,      // DX12 PIXEL_SHADING / Vk FRAGMENT_SHADER
    compute = 1u << 3,       // DX12 COMPUTE_SHADING / Vk COMPUTE_SHADER
    transfer = 1u << 4,      // copy/resolve: DX12 COPY / Vk (ALL_)TRANSFER
    attachment = 1u << 5,    // render-target + depth output: DX12 RENDER_TARGET|DEPTH_STENCIL / Vk COLOR|DS_ATTACHMENT
    raytracing = 1u << 6,    // DX12 RAYTRACING / Vk RAY_TRACING_SHADER
    accel_build = 1u << 7,   // DX12 BUILD_RAYTRACING_ACCELERATION_STRUCTURE / Vk ACCELERATION_STRUCTURE_BUILD
};

/// Memory layout a texture subresource is in. Buffers are always `general`. Maps to DX12
/// `BARRIER_LAYOUT` / Vulkan `ImageLayout`. Used only once textures land; carried now so the state model
/// is complete.
enum class texture_layout : u32
{
    undefined,    // no defined contents (discardable): DX12 LAYOUT_UNDEFINED / Vk IMAGE_LAYOUT_UNDEFINED
    general,      // buffers, and textures usable by any access: DX12 LAYOUT_COMMON / Vk IMAGE_LAYOUT_GENERAL
    shader_read,  // sampled/SRV: DX12 LAYOUT_SHADER_RESOURCE / Vk SHADER_READ_ONLY_OPTIMAL
    storage,      // UAV: DX12 LAYOUT_UNORDERED_ACCESS / Vk IMAGE_LAYOUT_GENERAL (storage)
    color,        // render target: DX12 LAYOUT_RENDER_TARGET / Vk COLOR_ATTACHMENT_OPTIMAL
    depth_read,   // DX12 LAYOUT_DEPTH_STENCIL_READ / Vk DEPTH_STENCIL_READ_ONLY_OPTIMAL
    depth_write,  // DX12 LAYOUT_DEPTH_STENCIL_WRITE / Vk DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    transfer_src, // copy source: DX12 LAYOUT_COPY_SOURCE / Vk TRANSFER_SRC_OPTIMAL
    transfer_dst, // copy dest: DX12 LAYOUT_COPY_DEST / Vk TRANSFER_DST_OPTIMAL
    present,      // swapchain present: DX12 LAYOUT_PRESENT / Vk PRESENT_SRC_KHR
};

// access_flags bit ops
[[nodiscard]] constexpr access_flags operator|(access_flags a, access_flags b)
{
    return access_flags(u32(a) | u32(b));
}
[[nodiscard]] constexpr access_flags operator&(access_flags a, access_flags b)
{
    return access_flags(u32(a) & u32(b));
}
[[nodiscard]] constexpr access_flags operator~(access_flags a)
{
    return access_flags(~u32(a));
}
constexpr access_flags& operator|=(access_flags& a, access_flags b)
{
    return a = a | b;
}

/// True if any bit is set.
[[nodiscard]] constexpr bool has_any(access_flags a)
{
    return u32(a) != 0;
}
/// True if every bit of `part` is set in `a`.
[[nodiscard]] constexpr bool has_all(access_flags a, access_flags part)
{
    return (u32(a) & u32(part)) == u32(part);
}
/// `a` with every bit of `remove` cleared.
[[nodiscard]] constexpr access_flags without(access_flags a, access_flags remove)
{
    return a & ~remove;
}

// pipeline_stage_flags bit ops
[[nodiscard]] constexpr pipeline_stage_flags operator|(pipeline_stage_flags a, pipeline_stage_flags b)
{
    return pipeline_stage_flags(u32(a) | u32(b));
}
[[nodiscard]] constexpr pipeline_stage_flags operator&(pipeline_stage_flags a, pipeline_stage_flags b)
{
    return pipeline_stage_flags(u32(a) & u32(b));
}
[[nodiscard]] constexpr pipeline_stage_flags operator~(pipeline_stage_flags a)
{
    return pipeline_stage_flags(~u32(a));
}
constexpr pipeline_stage_flags& operator|=(pipeline_stage_flags& a, pipeline_stage_flags b)
{
    return a = a | b;
}

[[nodiscard]] constexpr bool has_any(pipeline_stage_flags a)
{
    return u32(a) != 0;
}
[[nodiscard]] constexpr bool has_all(pipeline_stage_flags a, pipeline_stage_flags part)
{
    return (u32(a) & u32(part)) == u32(part);
}
[[nodiscard]] constexpr pipeline_stage_flags without(pipeline_stage_flags a, pipeline_stage_flags remove)
{
    return a & ~remove;
}

/// The accesses that constitute an *unordered write* — a write the hardware does not auto-serialize, so
/// a following access (read or write) needs an explicit barrier. Color/depth target writes are excluded:
/// they are ROP-ordered (globally serialized) and act as ordered "freebies".
[[nodiscard]] constexpr bool is_unordered_write(access_flags a)
{
    return has_any(a & (access_flags::shader_write | access_flags::transfer_write | access_flags::accel_write));
}
} // namespace sg
