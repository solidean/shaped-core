#pragma once

#include <clean-core/common/flags.hh>
#include <shaped-graphics/fwd.hh>

/// Small vocabulary enums shared across the shaped-graphics public API.

/// Coarse tag for the kind of backend behind a context — mainly to interpret raw handles from escape hatches.
/// Not the concrete type, and not exhaustive: debug, cpu or remote backends may exist, so never switch on it as a closed set.
enum class sg::backend_kind
{
    dx12,
    vulkan,
    metal,
    webgpu,
    opengl,
    webgl,
};

/// The threading guarantees a backend's context provides.
/// Coarse for now, and expected to gain nuance — e.g. whether concurrent command-list recording is allowed.
/// See libs/graphics/shaped-graphics/docs/concepts/threading.md.
enum class sg::thread_model
{
    single_threaded, ///< every context operation must be externally synchronized to one thread at a time
    multi_threaded,  ///< resource / command-list ops (create / submit / drop) are safe to call concurrently;
                     ///< epoch management (advance, waits) and shutdown must be externally synchronized
};

/// One way a buffer may be used across the pipeline.
/// A set of them is a `buffer_usages` — combine with `|`, test with `has`.
///
/// Names describe the *operation the buffer serves* at draw/dispatch time, not any one backend's flag.
/// Each value's trailing comment gives that per-backend mapping.
///
/// Fine-grained on purpose: a backend may need every usage declared at creation, and a distinction merged here cannot be recovered downstream.
/// A write-only buffer is deliberately not representable — that is a shader/binding access mode rather than a creation usage, and collapses into `readwrite_buffer`.
enum class sg::buffer_usage : sg::u32
{
    copy_src,                // Vk TRANSFER_SRC / WGPU COPY_SRC; DX12 & Metal implicit
    copy_dst,                // Vk TRANSFER_DST / WGPU COPY_DST; DX12 initial state; Metal implicit
    vertex_buffer,           // Vk VERTEX / WGPU VERTEX
    index_buffer,            // Vk INDEX / WGPU INDEX
    uniform_buffer,          // constant buffer: Vk UNIFORM / WGPU UNIFORM (size-capped)
    readonly_buffer,         // read-only structured/raw SRV: Vk STORAGE / WGPU STORAGE; DX12 no flag
    readwrite_buffer,        // UAV: Vk STORAGE / WGPU STORAGE; DX12 ALLOW_UNORDERED_ACCESS
    indirect_command_buffer, // Vk INDIRECT / WGPU INDIRECT

    // Acceleration-structure (raytracing). Vulkan also needs a buffer device address for these; the
    // backend adds it implicitly (no separate `device_address` usage for now).
    accel_structure_storage,     // Vk AS_STORAGE_KHR; DX12 AS resource state
    accel_structure_build_input, // Vk AS_BUILD_INPUT_READ_ONLY_KHR; DX12 plain SRV

    // Permits `stream_scope::region` on this buffer: the copy queue may transfer one byte range while the graphics
    // queue uses another, concurrently.
    // Vk CONCURRENT sharing over the graphics + transfer families (EXCLUSIVE cannot express two families at once);
    // DX12 / Metal / WGPU free, since a buffer has no layout and is already simultaneous-access capable.
    // Whole-resource streaming needs nothing anywhere, so this is only for streaming into a buffer something else is
    // reading at the same time.
    allow_region_stream,

    // Not yet modeled — add one when a backend needs it:
    // texel_buffer          — typed buffer view (Vk UNIFORM/STORAGE_TEXEL / DX12 typed SRV/UAV); a texture or a structured buffer covers most cases
    // device_address        — raw GPU pointer for pointer-based bindless; the accel-structure usages above already get it implicitly
    // conditional_rendering — Vk CONDITIONAL_RENDERING_EXT only (DX12 predication is implicit; no WGPU/Metal analogue)
    // stream_output         — transform feedback: Vk XFB EXT / DX12 stream-output
    // shader_binding_table  — raytracing SBT: gets its own abstraction, not a buffer usage
    // Cross-device sharing is absent on purpose: it is a memory property, so it belongs on memory_heap / allocation_info.
};

CC_FLAG_ENUM_INDEXED(sg, buffer_usage, u32);

namespace sg
{
/// A SET of buffer_usage — what a buffer is created with and reports, never the bare enum.
/// The empty set is legal and means no operation may touch the buffer, which every backend must still accept.
using buffer_usages = cc::flags<buffer_usage>;
} // namespace sg

/// One way a texture may be used across the pipeline.
/// A set of them is a `texture_usages` — combine with `|`, test with `has`.
///
/// Modeled at Vulkan's granularity, one flag per `VkImageUsageFlagBit`, since Vulkan needs every usage declared at creation and D3D12 is coarser.
/// Vulkan-only `INPUT_ATTACHMENT` / `TRANSIENT_ATTACHMENT` are omitted deliberately, having no D3D12 analogue.
enum class sg::texture_usage : sg::u32
{
    copy_src,          // Vk TRANSFER_SRC / WGPU COPY_SRC; DX12 implicit
    copy_dst,          // Vk TRANSFER_DST / WGPU COPY_DST; DX12 implicit
    readonly_texture,  // read-only sampled/SRV: Vk SAMPLED; DX12 no flag (default)
    readwrite_texture, // read-write UAV / storage image: Vk STORAGE; DX12 ALLOW_UNORDERED_ACCESS
    render_target,     // color attachment: Vk COLOR_ATTACHMENT; DX12 ALLOW_RENDER_TARGET
    depth_stencil,     // depth/stencil attachment: Vk DEPTH_STENCIL_ATTACHMENT; DX12 ALLOW_DEPTH_STENCIL

    // Permits `stream_scope::subresource`: the copy queue may transfer one whole mip / array slice while the graphics
    // queue uses a different one, concurrently.
    // Vk CONCURRENT sharing over the graphics + transfer families; DX12 free, since resource state is already tracked
    // per subresource.
    // Metal / WGPU free.
    allow_subresource_stream,

    // Permits `stream_scope::region` — a box INSIDE one subresource, while that same subresource is in use.
    // Implies allow_subresource_stream.
    // DX12 ALLOW_SIMULTANEOUS_ACCESS, which rules out depth/stencil and MSAA and can cost sampling bandwidth by
    // disabling metadata compression; Vk CONCURRENT sharing, with a similar risk.
    // Measure before reaching for it.
    allow_region_stream,
};

CC_FLAG_ENUM_INDEXED(sg, texture_usage, u32);

namespace sg
{
/// A SET of texture_usage — what a texture is created with and reports, never the bare enum.
using texture_usages = cc::flags<texture_usage>;
} // namespace sg

/// How much of a resource one streaming transfer claims exclusively, declared at the call.
///
/// The streaming contract is that the claimed extent is **yours alone** between the call and the moment the handle
/// reports complete: any command list touching it must be *submitted* after you observed that.
/// Declaring the scope is what lets the claim be checked — statically against the resource's usage flags, and
/// dynamically against what command lists actually touch.
///
/// Narrower is not automatically better.
/// `resource` is free on every backend and is the right choice for the common case of streaming into something
/// nothing else is looking at yet; the narrower scopes cost a creation-time usage flag, and on some backends real
/// sampling performance.
enum class sg::stream_scope : sg::u8
{
    /// Nothing else touches the resource at all while the transfer runs.
    /// Needs no usage flag anywhere — this is the prewarming case.
    resource,

    /// Other subresources (mips, array slices) may be used concurrently; this one is claimed whole.
    /// The canonical texture-streaming shape: filling mip 5 while mips 0-4 are sampled.
    /// Needs texture_usage::allow_subresource_stream, and is not meaningful for buffers, which have no subresources.
    subresource,

    /// Only the named byte range / box is claimed; the rest of that same subresource may be used concurrently.
    /// Needs buffer_usage::allow_region_stream or texture_usage::allow_region_stream.
    region,
};
