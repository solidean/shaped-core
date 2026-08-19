#pragma once

#include <clean-core/container/pinned_data.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>

/// Everything needed to build a compute_pipeline: the compiled compute `shader` and the `pipeline_layout` it is compiled against.
/// A struct rather than loose arguments, so it stays consistent with the graphics-pipeline descriptions to come and can grow a cached-pipeline field for PSO caching.
struct sg::compute_pipeline_description
{
    compiled_shader const& shader;
    pipeline_layout_handle layout;

    /// Optional serialized PSO blob for accelerated creation, skipping most shader-compile / driver work.
    /// Platform-specific and best-effort: a backend may ignore it.
    /// Empty by default; obtain one from a previously-built pipeline (cached_pipeline_data below) and persist it across runs.
    cc::pinned_data<byte const> cached_pipeline = {};
};

/// A ready-to-run compute pipeline: a compute shader compiled against a pipeline_layout.
/// Bound to a command list and dispatched.
/// Held via compute_pipeline_handle.
///
/// Abstract: a backend subclasses it and owns the native object (dx12 pipeline state + root signature,
/// vulkan VkPipeline + VkPipelineLayout). See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
class sg::compute_pipeline
{
public:
    virtual ~compute_pipeline();

    /// The shader's workgroup size (`[numthreads]` / `local_size`) — drives `cmd.compute.dispatch_threads`.
    [[nodiscard]] compute_dimensions workgroup_size() const { return _workgroup_size; }

    /// The backend's serialized PSO blob, for persisting and feeding back via compute_pipeline_description::cached_pipeline.
    /// Empty if the backend does not support it.
    [[nodiscard]] virtual cc::pinned_data<byte const> cached_pipeline_data() const = 0;

    /// Whether creation actually consumed the `cached_pipeline` blob it was handed.
    ///
    /// False when none was supplied, and false when one was supplied and the backend rejected it.
    /// A rejection is the exact signal that a persisted blob has gone stale — a driver update, a different adapter —
    /// so a cache replaces its entry on it rather than trying to predict staleness through the key.
    [[nodiscard]] bool used_cached_pipeline() const { return _used_cached_pipeline; }

protected:
    explicit compute_pipeline(compute_dimensions workgroup_size) : _workgroup_size(workgroup_size) {}

    compute_dimensions _workgroup_size;

    /// Set by the backend during creation; see used_cached_pipeline().
    bool _used_cached_pipeline = false;
};
