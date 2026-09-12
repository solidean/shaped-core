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
};

/// Whether a caller may block on this context at all.
///
/// The rule the whole API is shaped around: **an async call may be converted to a blocking one only where the wait
/// amortizes over many operations.**
/// Per-frame yes, per-startup-batch yes, per-object never.
/// So sg is never-blocking by default everywhere, and the `block_until_` prefix is the complete inventory of the
/// exceptions — the spellings that admit to waiting, and the ones a target that cannot wait refuses.
/// There are exactly two, `block_until_epochs_in_flight()` and `block_until_idle()`; libs/graphics/shaped-graphics/docs/concepts/epochs.md has the pair.
///
/// A browser cannot wait at all: a promise settles only after the current task's stack unwinds, so a loop waiting on a
/// callback has taken the only thread that callback could run on.
/// That is a property of the target rather than a caller's choice, which is why this is reported and not set.
enum class sg::execution_model
{
    /// A caller may block: both `block_until_` spellings work, and a test or a tool can drain the device and read the
    /// result.
    may_block,

    /// Nothing may block, and both `block_until_` spellings assert.
    /// Completion is observed through the `*_completion()` asyncs, or by polling across frames.
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
