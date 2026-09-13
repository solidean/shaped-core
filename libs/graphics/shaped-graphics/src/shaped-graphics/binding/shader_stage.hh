#pragma once

#include <clean-core/common/flags.hh>
#include <shaped-graphics/fwd.hh>

/// The pipeline stages a shader can run at, and a set of them.
///
/// This lives apart from compiled_shader.hh because a `binding` carries the set of stages that declared it, and
/// compiled_shader.hh already includes binding.hh — so the stage vocabulary has to sit below both.

/// Pipeline stage a shader runs at.
/// Compute and the ray-tracing stages are wired; the graphics stages fill in as pipelines land.
enum class sg::shader_stage
{
    vertex,
    tessellation_control,    ///< dx12 hull (hs): the patch-constant + per-control-point stage
    tessellation_evaluation, ///< dx12 domain (ds): evaluates the tessellated surface
    geometry,                ///< dx12 geometry (gs): per-primitive stage that may amplify/emit primitives
    fragment,
    compute,
    // Ray-tracing stages — each compiles to a single-entry `lib_6_x` blob (no dedicated shader-library type).
    raygen,
    closest_hit,
    any_hit,
    miss,
    intersection,
    callable,
    // Future: mesh, task, ...
};

CC_FLAG_ENUM_INDEXED(sg, shader_stage, cc::u16);

namespace sg
{
/// A SET of shader_stage — which stages a binding is visible to.
/// The empty set means "not known", not "no stage": a hand-written binding that never says is treated as visible
/// everywhere, because refusing it would be a worse guess than being permissive.
using shader_stages = cc::flags<shader_stage>;

/// True for the six ray-tracing stages (raygen / closest_hit / any_hit / miss / intersection / callable).
[[nodiscard]] constexpr bool is_raytracing_stage(shader_stage s)
{
    switch (s)
    {
    case shader_stage::raygen:
    case shader_stage::closest_hit:
    case shader_stage::any_hit:
    case shader_stage::miss:
    case shader_stage::intersection:
    case shader_stage::callable:
        return true;
    default:
        return false;
    }
}

/// True for the compute stage.
[[nodiscard]] constexpr bool is_compute_stage(shader_stage s)
{
    return s == shader_stage::compute;
}
} // namespace sg
