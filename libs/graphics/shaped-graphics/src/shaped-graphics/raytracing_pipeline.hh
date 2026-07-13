#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>

namespace sg
{
/// The shaders of one hit group. `closest_hit` and `any_hit` run for triangle geometry; `intersection`
/// makes the group procedural (custom primitives). Presence of `intersection` picks the hit-group type:
/// a triangle BLAS must not run a group with an intersection shader, and a procedural BLAS must run one
/// that has it — mismatches are undefined behavior the backend may not catch.
struct hit_shader
{
    cc::optional<compiled_shader> closest_hit;
    cc::optional<compiled_shader> any_hit;
    cc::optional<compiled_shader> intersection;
};

/// Everything needed to build a raytracing_pipeline: the pipeline_layout (its global root signature) plus
/// the shaders that make up the state object, grouped by category. Unlike compute_pipeline_description this
/// owns its shaders (a pipeline combines several), so building one on a worker thread is safe.
///
/// Register shaders with the `add_*` helpers, which return the handle to reference the shader by (its slot
/// in the matching vector) — the same order a raytracing_shader_table later maps to table indices.
struct raytracing_pipeline_description
{
    pipeline_layout_handle layout;

    cc::vector<compiled_shader> raygen_shaders;
    cc::vector<compiled_shader> miss_shaders;
    cc::vector<compiled_shader> callable_shaders;
    cc::vector<hit_shader> hit_shaders;

    /// Maximum TraceRay recursion depth the pipeline may reach (>= 1). Keep it as low as the shaders need.
    cc::u32 max_recursion_depth = 1;
    /// Maximum ray-payload size in bytes (the inout struct passed through TraceRay).
    cc::isize max_payload_size = 0;
    /// Maximum hit-attribute size in bytes (8 fits the built-in barycentrics of triangle intersection).
    cc::isize max_attribute_size = 8;

    /// Optional serialized state-object blob for accelerated creation. Best-effort; backends may ignore it.
    cc::pinned_data<cc::byte const> cached_pipeline = {};

    /// Registers a raygen shader; asserts `shader.stage == raygen`. Returns its handle.
    [[nodiscard]] raygen_shader_handle add_raygen_shader(compiled_shader shader);
    /// Registers a miss shader; asserts `shader.stage == miss`. Returns its handle.
    [[nodiscard]] miss_shader_handle add_miss_shader(compiled_shader shader);
    /// Registers a callable shader; asserts `shader.stage == callable`. Returns its handle.
    [[nodiscard]] callable_shader_handle add_callable_shader(compiled_shader shader);
    /// Registers a hit group; asserts each present member has the matching stage. Returns its handle.
    [[nodiscard]] hit_shader_handle add_hit_shader(hit_shader shader);
};

/// A ready-to-trace ray-tracing pipeline built from a raytracing_pipeline_description. Bind it with
/// cmd.raytracing.bind_pipeline and dispatch it via cmd.raytracing.dispatch_rays through a
/// raytracing_shader_table. Held via raytracing_pipeline_handle.
class raytracing_pipeline
{
public:
    virtual ~raytracing_pipeline();

    /// The backend's serialized state-object blob, for persisting and feeding back via
    /// raytracing_pipeline_description::cached_pipeline. Empty if the backend doesn't support it.
    [[nodiscard]] virtual cc::pinned_data<cc::byte const> cached_pipeline_data() const = 0;

protected:
    raytracing_pipeline() = default;
};
} // namespace sg
