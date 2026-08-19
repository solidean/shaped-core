#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>

/// The shaders of one hit group.
/// `closest_hit` and `any_hit` run for triangle geometry; `intersection` makes the group procedural, for custom primitives.
/// Whether `intersection` is present picks the hit-group type: a triangle BLAS must not run a group that has one, and a procedural BLAS must.
/// A mismatch is undefined behavior the backend may not catch.
struct sg::hit_shader
{
    cc::optional<compiled_shader> closest_hit;
    cc::optional<compiled_shader> any_hit;
    cc::optional<compiled_shader> intersection;
};

/// Everything needed to build a raytracing_pipeline: the pipeline_layout, its global root signature, plus the state object's shaders grouped by category.
/// Unlike compute_pipeline_description this owns its shaders, since a pipeline combines several, so building one on a worker thread is safe.
///
/// Register shaders with the `add_*` helpers, which return the handle to reference the shader by — its slot in the matching vector.
/// That is the order a raytracing_shader_table later maps to table indices.
struct sg::raytracing_pipeline_description
{
    pipeline_layout_handle layout;

    cc::vector<compiled_shader> raygen_shaders;
    cc::vector<compiled_shader> miss_shaders;
    cc::vector<compiled_shader> callable_shaders;
    cc::vector<hit_shader> hit_shaders;

    /// Maximum TraceRay recursion depth the pipeline may reach, which must be >= 1.
    /// Keep it as low as the shaders need.
    u32 max_recursion_depth = 1;
    /// Maximum ray-payload size in bytes (the inout struct passed through TraceRay).
    isize max_payload_size = 0;
    /// Maximum hit-attribute size in bytes; 8 fits the built-in barycentrics of triangle intersection.
    isize max_attribute_size = 8;

    /// Optional serialized state-object blob for accelerated creation.
    /// Best-effort — backends may ignore it.
    cc::pinned_data<byte const> cached_pipeline = {};

    /// Registers a raygen shader and returns its handle; asserts `shader.stage == raygen`.
    [[nodiscard]] raygen_shader_handle add_raygen_shader(compiled_shader shader);
    /// Registers a miss shader and returns its handle; asserts `shader.stage == miss`.
    [[nodiscard]] miss_shader_handle add_miss_shader(compiled_shader shader);
    /// Registers a callable shader and returns its handle; asserts `shader.stage == callable`.
    [[nodiscard]] callable_shader_handle add_callable_shader(compiled_shader shader);
    /// Registers a hit group and returns its handle; asserts each present member has the matching stage.
    [[nodiscard]] hit_shader_handle add_hit_shader(hit_shader shader);
};

/// A ready-to-trace ray-tracing pipeline, built from a raytracing_pipeline_description and held via raytracing_pipeline_handle.
/// Bind it with cmd.raytracing.bind_pipeline, and dispatch it via cmd.raytracing.dispatch_rays through a raytracing_shader_table.
class sg::raytracing_pipeline
{
public:
    virtual ~raytracing_pipeline();

    /// The backend's serialized state-object blob, for persisting and feeding back via raytracing_pipeline_description::cached_pipeline.
    /// Empty if the backend does not support it.
    [[nodiscard]] virtual cc::pinned_data<byte const> cached_pipeline_data() const = 0;

    /// Whether creation actually consumed the `cached_pipeline` blob it was handed.
    ///
    /// False when none was supplied, and false when one was supplied and the backend rejected it.
    /// A rejection is the exact signal that a persisted blob has gone stale — a driver update, a different adapter —
    /// so a cache replaces its entry on it rather than trying to predict staleness through the key.
    ///
    /// Always false on a backend whose state objects carry no blob at all, dx12 among them, which is the same answer
    /// a caller needs: there is nothing worth persisting here.
    [[nodiscard]] bool used_cached_pipeline() const { return _used_cached_pipeline; }

protected:
    raytracing_pipeline() = default;

    /// Set by the backend during creation; see used_cached_pipeline().
    bool _used_cached_pipeline = false;
};
