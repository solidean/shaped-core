#pragma once

#include <clean-core/common/flags.hh>
#include <shaped-graphics/fwd.hh>

/// Backend-neutral vocabulary for resource access tracking.
/// How a GPU operation touches a resource (`access_flags`), in which pipeline stages (`pipeline_stage_flags`), and — for textures — the memory layout it needs (`texture_layout`).
/// Deliberately not any single backend's spelling: each value's trailing comment gives its D3D12 and Vulkan mapping.
///
/// These are shared, opt-in building blocks.
/// A backend that emits explicit barriers tracks state in terms of them (see resource_access_state.hh); one that relies on driver-managed barriers may ignore them entirely.
/// Buffers only ever use the `general` layout — layouts matter for textures.
/// See libs/graphics/shaped-graphics/docs/concepts/barriers.md.

/// One thing a GPU operation does to a resource.
/// A set of them is an `access_flags` — combine with `|`, test with `has` / `has_any` / `has_all`.
///
/// Read vs write is explicit in the suffix.
/// `is_unordered_write` marks the accesses that create a hazard needing a barrier — shader, transfer and accel writes.
/// Color/depth *target* writes are ROP-ordered by the hardware, and are not unordered.
enum class sg::access_flag : sg::u32
{
    uniform_read,  // constant-buffer read:   DX12 CONSTANT_BUFFER / Vk UNIFORM_READ
    index_read,    // index-buffer fetch:      DX12 INDEX_BUFFER    / Vk INDEX_READ
    vertex_read,   // vertex-buffer fetch:     DX12 VERTEX_BUFFER   / Vk VERTEX_ATTRIBUTE_READ
    shader_read,   // SRV / sampled+storage:   DX12 SHADER_RESOURCE / Vk SHADER_READ
    shader_write,  // UAV / storage write:     DX12 UNORDERED_ACCESS/ Vk SHADER_WRITE
    copy_read,     // copy/resolve source:     DX12 COPY_SOURCE     / Vk TRANSFER_READ
    copy_write,    // copy/resolve dest:       DX12 COPY_DEST       / Vk TRANSFER_WRITE
    indirect_read, // indirect args:           DX12 INDIRECT_ARGUMENT / Vk INDIRECT_COMMAND_READ

    // Texture / render-target / raytracing families.
    // A buffer only ever uses the accel_* pair, through cmd.raytracing.
    color_write, // render-target write:     DX12 RENDER_TARGET   / Vk COLOR_ATTACHMENT_WRITE
    depth_read,  // depth/stencil test:      DX12 DEPTH_STENCIL_READ  / Vk DEPTH_STENCIL_ATTACHMENT_READ
    depth_write, // depth/stencil write:    DX12 DEPTH_STENCIL_WRITE / Vk DEPTH_STENCIL_ATTACHMENT_WRITE
    accel_read,  // AS read/trace:          DX12 / Vk ACCELERATION_STRUCTURE_READ
    accel_write, // AS build:               DX12 / Vk ACCELERATION_STRUCTURE_WRITE
};

CC_FLAG_ENUM_INDEXED(sg, access_flag, u32);

/// One pipeline stage that may perform an access.
/// A set of them is a `pipeline_stage_flags` — combine with `|`, test with `has` / `has_any` / `has_all`.
/// Coarse on purpose — tessellation/geometry fold into `vertex`, early/late depth into `depth_stencil_target`.
/// That mirrors how DX12 `BARRIER_SYNC` and Vulkan `PIPELINE_STAGE_2` are typically consumed.
enum class sg::pipeline_stage_flag : sg::u32
{
    draw_indirect,        // DX12 EXECUTE_INDIRECT / Vk DRAW_INDIRECT
    vertex,               // vertex-processing stages: DX12 VERTEX_SHADING / Vk VERTEX_SHADER (+ pre-raster)
    fragment,             // DX12 PIXEL_SHADING / Vk FRAGMENT_SHADER
    compute,              // DX12 COMPUTE_SHADING / Vk COMPUTE_SHADER
    copy,                 // copy/resolve: DX12 COPY / Vk (ALL_)TRANSFER
    render_target,        // color output:         DX12 RENDER_TARGET / Vk COLOR_ATTACHMENT_OUTPUT
    depth_stencil_target, // depth/stencil output: DX12 DEPTH_STENCIL / Vk EARLY|LATE_FRAGMENT_TESTS
    raytracing,           // DX12 RAYTRACING / Vk RAY_TRACING_SHADER
    accel_build,          // DX12 BUILD_RAYTRACING_ACCELERATION_STRUCTURE / Vk ACCELERATION_STRUCTURE_BUILD
};

CC_FLAG_ENUM_INDEXED(sg, pipeline_stage_flag, u32);

namespace sg
{
/// A SET of access_flag — what an op declares and a state machine accumulates, never the bare enum.
using access_flags = cc::flags<access_flag>;

/// A SET of pipeline_stage_flag — the stages one declared access spans.
using pipeline_stage_flags = cc::flags<pipeline_stage_flag>;
} // namespace sg

/// Memory layout a texture subresource is in; buffers are always `general`.
/// Maps to DX12 `BARRIER_LAYOUT` / Vulkan `ImageLayout`.
/// Live for textures today — dx12 transitions render targets, shader reads and copy destinations through it.
enum class sg::texture_layout : sg::u32
{
    undefined,        // no defined contents (discardable): DX12 LAYOUT_UNDEFINED / Vk IMAGE_LAYOUT_UNDEFINED
    general,          // buffers, and textures usable by any access: DX12 LAYOUT_COMMON / Vk IMAGE_LAYOUT_GENERAL
    shader_readonly,  // sampled/SRV: DX12 LAYOUT_SHADER_RESOURCE / Vk SHADER_READ_ONLY_OPTIMAL
    shader_readwrite, // UAV / storage: DX12 LAYOUT_UNORDERED_ACCESS / Vk IMAGE_LAYOUT_GENERAL
    render_target,    // color attachment: DX12 LAYOUT_RENDER_TARGET / Vk COLOR_ATTACHMENT_OPTIMAL
    depth_readonly,   // DX12 LAYOUT_DEPTH_STENCIL_READ / Vk DEPTH_STENCIL_READ_ONLY_OPTIMAL
    depth_readwrite,  // DX12 LAYOUT_DEPTH_STENCIL_WRITE / Vk DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    copy_src,         // copy source: DX12 LAYOUT_COPY_SOURCE / Vk TRANSFER_SRC_OPTIMAL
    copy_dst,         // copy dest: DX12 LAYOUT_COPY_DEST / Vk TRANSFER_DST_OPTIMAL
    present,          // swapchain present: DX12 LAYOUT_PRESENT / Vk PRESENT_SRC_KHR
};

/// Which way an async / streaming transfer of a texture goes, for `cmd.prepare_for_async`.
/// The layout it resolves to is the backend's, through `context::async_ready_layout`.
///
/// **Every backend answers `general` today, whichever way the transfer goes**, so the direction is recorded and then
/// ignored.
/// It is kept because a direction-specific layout is postponed rather than ruled out: vulkan would keep more
/// compression with one, the shape extends to that easily, and it would be hard to add back once dropped.
/// libs/graphics/shaped-graphics/docs/TODO.md carries what taking it up costs.
enum class sg::async_direction : sg::u32
{
    upload,   ///< host -> texture
    download, ///< texture -> host
    /// Both, which forgoes whatever compression a specialized layout would keep once directions are honoured.
    /// Explicit only, and never a default — the trade is the caller's to make once they know they do both.
    both,
};

namespace sg
{
/// The accesses that constitute an *unordered write* — one the hardware does not auto-serialize, so a following access, read or write, needs an explicit barrier.
/// Color/depth target writes are excluded: they are ROP-ordered (globally serialized) and act as ordered freebies.
inline constexpr access_flags unordered_write_accesses
    = access_flag::shader_write | access_flag::copy_write | access_flag::accel_write;

/// True if `a` contains an unordered write.
[[nodiscard]] constexpr bool is_unordered_write(access_flags a)
{
    return a.has_any(unordered_write_accesses);
}

/// Every access that only observes the resource.
/// An op can carry both halves at once — a copy whose source and destination are the same resource does.
inline constexpr access_flags read_accesses
    = access_flag::uniform_read | access_flag::index_read | access_flag::vertex_read | access_flag::shader_read
    | access_flag::copy_read | access_flag::indirect_read | access_flag::depth_read | access_flag::accel_read;

/// True if `a` observes the resource at all, whatever else it does to it.
[[nodiscard]] constexpr bool has_read_access(access_flags a)
{
    return a.has_any(read_accesses);
}
} // namespace sg
