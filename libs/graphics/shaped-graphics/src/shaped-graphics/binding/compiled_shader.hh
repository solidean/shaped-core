#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/shader_stage.hh>
#include <shaped-graphics/fwd.hh>

/// A compiled shader: a bytecode blob plus the metadata and reflection needed to build pipelines and bind resources.
/// sg only consumes one — producing it is shaped-shader-library's job, through a compiler such as shaped-shader-compiler-dxc.
/// See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
/// libs/graphics/shaped-graphics/docs/shaders.md has the shader path end to end.
///
/// `sg::shader_stage` and its set live in binding/shader_stage.hh, below this header and below binding.hh, because a
/// reflected binding carries the stages that declared it.

/// Bytecode format of the blob — which backend can consume it.
/// A backend-agnostic shader must record it, so a pipeline knows whether the blob is for it.
enum class sg::shader_format
{
    dxil,      ///< DirectX Intermediate Language — dx12
    spirv,     ///< SPIR-V — vulkan (and others)
    metal_lib, ///< Metal library — metal
    // WGSL is SOURCE text rather than bytecode: WebGPU consumes it and compiles it itself.
    wgsl, ///< WGSL — webgpu
    // Future: dxbc.
};

/// Provenance of the compile — mostly a cache-invalidation / debugging aid.
/// `signature` is a free-form string capturing the flags/defines/source identity a compiler folds into its cache key.
struct sg::compiler_info
{
    cc::string name;      ///< e.g. "dxc"
    cc::string version;   ///< compiler version
    cc::string signature; ///< opaque provenance (options / defines / source hash)
};

/// A compute shader's `[numthreads]` / `local_size` — the workgroup dimensions.
struct sg::compute_dimensions
{
    int x = 1;
    int y = 1;
    int z = 1;
};

/// A successfully compiled shader: the bytecode blob and its extracted metadata + reflection, ready to build a pipeline from or cache.
/// Reflection (the `bindings`) is stored inline.
/// A pure value; share it via compiled_shader_handle.
struct sg::compiled_shader
{
    shader_stage stage = shader_stage::compute;
    shader_format format = shader_format::dxil;
    cc::string entry_point;

    /// The opaque bytecode, in `format`. An owning, shareable, immutable byte blob.
    cc::pinned_data<byte const> bytecode;

    /// Reflected resource bindings — a flat list; per-set grouping is derived by the consumer.
    cc::vector<binding> bindings;

    /// Compute workgroup size, present only for a compute `stage`.
    cc::optional<compute_dimensions> workgroup_size;

    compiler_info compiler;

    // Deferred: constant-buffer member layouts, root/push constants, content hash, I/O signatures.
};
