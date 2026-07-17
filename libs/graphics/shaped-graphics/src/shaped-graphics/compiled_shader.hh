#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/fwd.hh>

/// A compiled shader: a bytecode blob plus the metadata and reflection needed to build pipelines and
/// bind resources — produced by a future compiler/loader (compilation is not part of sg yet). See
/// libs/graphics/shaped-graphics/docs/concepts/bindings.md.

namespace sg
{
/// Pipeline stage a shader runs at. Compute and the ray-tracing stages are wired; the graphics stages
/// fill in as pipelines land.
enum class shader_stage
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

/// Bytecode format of the blob — which backend can consume it. A backend-agnostic shader must record it,
/// so a pipeline knows whether the blob is for it.
enum class shader_format
{
    dxil,      ///< DirectX Intermediate Language — dx12
    spirv,     ///< SPIR-V — vulkan (and others)
    metal_lib, ///< Metal library — metal
    // Future: dxbc, wgsl.
};

/// Provenance of the compile — mostly a cache-invalidation / debugging aid. `signature` is a free-form
/// string capturing the flags/defines/source identity a compiler folds into its cache key.
struct compiler_info
{
    cc::string name;      ///< e.g. "dxc"
    cc::string version;   ///< compiler version
    cc::string signature; ///< opaque provenance (options / defines / source hash)
};

/// A compute shader's `[numthreads]` / `local_size` — the workgroup dimensions.
struct compute_dimensions
{
    int x = 1;
    int y = 1;
    int z = 1;
};

/// A successfully compiled shader: the bytecode blob and its extracted metadata + reflection, ready to
/// build a pipeline from or cache. Reflection (the `bindings`) is stored inline. A pure value; share it
/// via compiled_shader_handle.
struct compiled_shader
{
    shader_stage stage = shader_stage::compute;
    shader_format format = shader_format::dxil;
    cc::string entry_point;

    /// The opaque bytecode, in `format`. An owning, shareable, immutable byte blob.
    cc::pinned_data<cc::byte const> bytecode;

    /// Reflected resource bindings — a flat list; per-set grouping is derived by the consumer.
    cc::vector<binding> bindings;

    /// Compute workgroup size, present only for a compute `stage`.
    cc::optional<compute_dimensions> workgroup_size;

    compiler_info compiler;

    // Deferred: constant-buffer member layouts, root/push constants, content hash, I/O signatures.
};
} // namespace sg
