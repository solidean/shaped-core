#pragma once

#include <shaped-graphics/fwd.hh>

/// What a context can and cannot do, as one vocabulary rather than one spelling per question.
///
/// The set is deliberately small and coarse.
/// Every added granularity is a new way for a renderer to be non-portable without noticing: a caller that branches on
/// a fine-grained capability is a caller that behaves differently per backend, which is the thing sg exists to avoid.
/// So a `feature` earns its place only when a whole code path is present or absent, never to describe a difference of
/// degree.

/// A capability a context either has or does not.
/// Absent means the code path does not exist on this backend or device — not that it is slow.
enum class sg::feature
{
    /// Ray tracing: acceleration structures, ray-tracing pipelines, inline RayQuery.
    /// A device fact as much as a backend one, since an adapter may lack DXR / VK_KHR_ray_tracing_pipeline.
    raytracing,

    /// GPU timestamp queries (`cmd.query.record_gpu_timestamp`).
    /// Absent where the queue family does not time, and an optional feature on WebGPU.
    timestamp_query,

    /// A swapchain may be created with a `headless_extent` and no window.
    headless_present,

    /// The geometry stage exists and a raster pipeline may declare one.
    /// WebGPU has no geometry stage at all, which is what makes this worth asking before building a pipeline.
    geometry_shader,

    /// The tessellation control + evaluation stages exist.
    /// Both or neither, which is why they are one feature rather than two.
    tessellation_shader,

    /// A binding may be an array (`count > 1`), and so `staging_binding_group` and `bindless_array` work.
    /// WebGPU core has no binding arrays at all, so a bindless renderer asks once rather than finding out at layout creation.
    binding_arrays,

    /// A storage texture may be `read_write` in any image format, not only r32float / r32uint / r32sint.
    /// WebGPU core allows read-write on those three alone, and its `texture-formats-tier2` lifts that; write-only and read-only work everywhere.
    readwrite_image_formats,

    /// A sampled texture of `r32_float`, `rg32_float` or `rgba32_float` may be filtered.
    /// Core WebGPU makes those three unfilterable and its `float32-filterable` lifts that, and Vulkan reports it per format.
    /// Without it, binding such a view to a `filterable_float` binding is refused on every backend.
    float32_filtering,

    /// A storage texture may use a format outside `sg::is_portable_image_format`, such as `r8_unorm` or `rgb10a2_unorm`.
    /// `bgra8_unorm` is among them, and it is the one each API asks for on its own.
    /// WebGPU grants them with `texture-formats-tier1` and `bgra8unorm-storage`.
    /// Vulkan grants them with `shaderStorageImageExtendedFormats` plus `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT` on `B8G8R8A8_UNORM`.
    /// D3D12 grants them with a typed UAV on `DXGI_FORMAT_B8G8R8A8_UNORM`, the rest being required at feature level 11_0.
    extended_image_formats,

    /// A block-compressed texture may have a width or height that is no multiple of its block (4 for BC).
    /// WebGPU core refuses one unless the device has `texture-compression-unaligned`, and D3D12 reports it as an option.
    /// Where this is false, creating one is a refusal naming the size, which a loader of user textures can pad against.
    unaligned_block_compression,
};

/// Whether the thread driving this context may block at all.
///
/// The rule the whole API is shaped around: **an async call may be converted to a blocking one only where the wait
/// amortizes over many operations.**
/// Per-frame yes, per-startup-batch yes, per-object never.
/// sg itself hands a caller no blocking spelling: a frame loop awaits `epochs_in_flight_completion()`, a drain awaits `idle_completion()`.
/// Whether the caller then blocks on one of those is its own decision, and this is what it asks first.
///
/// A browser cannot wait at all: a promise settles only after the current task's stack unwinds, so a loop waiting on a
/// callback has taken the only thread that callback could run on.
/// That is a property of the target rather than a caller's choice, which is why this is reported and not set.
enum class sg::execution_model
{
    /// A caller may block: a tool can `cc::async_blocking_get` a completion, and sg's own internal waits run.
    may_block,

    /// Nothing may block: completion is observed by awaiting the `*_completion()` asyncs, or by polling across frames.
    never_block,
};

/// Numeric bounds a portable caller has to stay inside.
///
/// Every field is a floor a backend guarantees rather than the most the hardware could do: a caller sizing against it
/// is portable by construction, and a backend that has not measured reports the portable floor rather than a guess.
struct sg::device_limits
{
    /// Group slots a pipeline layout may hand a caller — see sg::max_binding_groups, whose reservation this reports.
    int max_binding_groups = sg::max_binding_groups;

    /// The highest `sample_count` a texture description may ask for.
    /// 1 means no multisampling; WebGPU allows only 1 and 4, which is the floor everything else clears.
    int max_sample_count = 1;
};
