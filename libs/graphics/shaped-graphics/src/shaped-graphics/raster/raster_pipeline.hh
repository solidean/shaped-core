#pragma once

#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raster/blend_state.hh>
#include <shaped-graphics/raster/depth_stencil_state.hh>
#include <shaped-graphics/raster/primitive_topology.hh>
#include <shaped-graphics/raster/rasterization_state.hh>
#include <shaped-graphics/raster/vertex_input.hh>
#include <shaped-graphics/resource/pixel_format.hh>

/// The PSO-side state of one color target: the format the pipeline is compiled for, its optional blend equation, and which channels it writes.
/// The command-side counterpart is `color_target`, the actual bound view plus clear/discard — the two must agree on format and order at draw time.
struct sg::color_target_state
{
    pixel_format format = pixel_format::undefined;
    cc::optional<blend_state> blend = {}; ///< unset => the source overwrites the target (no blend)
    color_write_mask write_mask = color_write_mask_all;
};

/// Everything needed to build a raster_pipeline: the shaders, the pipeline_layout they are compiled against, the vertex-input layout, and the fixed-function state.
/// It owns its shaders, since a pipeline combines several and building on a worker thread must be safe — parity with raytracing_pipeline_description.
///
/// The color-target formats and sample count live here, not only in the rendering scope, because backends bake them into the PSO.
/// That is dx12 RTVFormats / DSVFormat, and vulkan's dynamic-rendering formats.
/// A rendering scope's bound targets must match `color_targets` in count and format, and `depth_stencil_format`.
struct sg::raster_pipeline_description
{
    pipeline_layout_handle layout;

    compiled_shader vertex_shader;                 ///< the vertex stage (required)
    cc::optional<compiled_shader> fragment_shader; ///< the fragment stage; omit for a depth-only pipeline

    /// Optional tessellation stages, both set together or both omitted — a tessellator needs a hull *and* a domain stage.
    /// When set, `topology` must be `patch_list` and `patch_control_points` > 0.
    cc::optional<compiled_shader> tessellation_control_shader;    ///< hull stage
    cc::optional<compiled_shader> tessellation_evaluation_shader; ///< domain stage

    /// Optional geometry stage — a per-primitive shader that may amplify or emit primitives.
    /// Omit it for none.
    cc::optional<compiled_shader> geometry_shader;

    vertex_input_layout vertex_input;
    primitive_topology topology = primitive_topology::triangle_list;

    /// Control points per patch, used only when `topology == patch_list` and ignored otherwise.
    /// Must be 1..32.
    int patch_control_points = 0;

    rasterization_state rasterization = {};
    depth_stencil_state depth_stencil = {};

    /// The color targets the pipeline writes, in output-merger order.
    /// Empty for a depth-only pipeline.
    cc::fixed_vector<color_target_state, max_color_targets> color_targets;

    /// The depth-stencil target's format, or `undefined` for no depth-stencil attachment.
    pixel_format depth_stencil_format = pixel_format::undefined;

    /// MSAA sample count of the targets (1 = no multisampling).
    int sample_count = 1;

    /// Optional serialized PSO blob for accelerated creation, skipping most driver work.
    /// Platform-specific and best-effort: a backend may ignore it.
    /// Obtain one from a previously-built pipeline via `cached_pipeline_data()` and persist it across runs.
    cc::pinned_data<byte const> cached_pipeline = {};
};

/// A ready-to-draw raster (graphics) pipeline: vertex + fragment shaders compiled against a pipeline_layout, with a fixed-function state set.
/// Bound with cmd.raster.bind_pipeline and drawn inside a rendering scope.
/// Held via raster_pipeline_handle.
///
/// Abstract: a backend subclasses it and owns the native object (dx12 pipeline state + root signature,
/// vulkan VkPipeline + VkPipelineLayout).
class sg::raster_pipeline
{
public:
    virtual ~raster_pipeline();

    /// Destroys the backend objects now, leaving this an inert husk.
    ///
    /// The context calls this at shutdown for everything its pipeline cache still holds, because the cache dropping
    /// its own reference is NOT what frees them: a scheduled build node keeps the handle alive on a pool worker, and
    /// that node's later drop would run this destructor against a context that no longer exists.
    /// See libs/graphics/shaped-graphics/docs/concepts/caches.md, "Shutdown releases the objects, not just the entries".
    ///
    /// Idempotent, and the destructor afterwards does nothing.
    /// The default is empty, for a backend whose objects are reference-counted and safe to drop late.
    virtual void release_backend_objects() {}

    /// The backend's serialized PSO blob, for persisting and feeding back via raster_pipeline_description::cached_pipeline.
    /// Empty if the backend does not support it.
    [[nodiscard]] virtual cc::pinned_data<byte const> cached_pipeline_data() const = 0;

    /// Whether creation actually consumed the `cached_pipeline` blob it was handed.
    ///
    /// False when none was supplied, and false when one was supplied and the backend rejected it.
    /// A rejection is the exact signal that a persisted blob has gone stale — a driver update, a different adapter —
    /// so a cache replaces its entry on it rather than trying to predict staleness through the key.
    [[nodiscard]] bool used_cached_pipeline() const { return _used_cached_pipeline; }

protected:
    raster_pipeline() = default;

    /// Set by the backend during creation; see used_cached_pipeline().
    bool _used_cached_pipeline = false;
};
