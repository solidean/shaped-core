#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/barrier/footprint.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/debug_site.hh>
#include <shaped-graphics/binding/shader_stage.hh>
#include <shaped-graphics/context/capabilities.hh>
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
    /// Metal Shading Language source — metal, compiled by the driver when a pipeline is built.
    ///
    /// The second source format, and it exists for the same reason `wgsl` does: producing a metallib needs Apple's
    /// Metal toolchain, a component installed separately from Xcode, while the driver's own compiler ships with the OS.
    /// So a host without that component emits this instead, and a shipping build asks for `metal_lib`.
    /// New values go after this one: `impl/shader_codec.cc` encodes a format by its ordinal.
    msl,
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

/// A successfully compiled shader: the bytecode blob and its metadata, ready to build a pipeline from or cache.
/// For HLSL and WGSL the metadata is the compiler's reflection; an SGL shader states all of it itself, and its compiler contributes only `bytecode` and `compiler`.
/// A pure value; share it via compiled_shader_handle.
struct sg::compiled_shader
{
    shader_stage stage = shader_stage::compute;
    shader_format format = shader_format::dxil;
    cc::string entry_point;

    /// The opaque bytecode, in `format`.
    /// An owning, shareable, immutable byte blob.
    cc::pinned_data<byte const> bytecode;

    /// The resource bindings the code uses — a flat list; per-set grouping is derived by the consumer.
    cc::vector<binding> bindings;

    /// The workgroup size of a compute, mesh or task `stage`, which Metal states at the dispatch or the mesh draw.
    cc::optional<compute_dimensions> workgroup_size;
    /// The subgroup size a compute shader runs best at, which a backend asks the device for where it can and ignores
    /// where it cannot: a preference, never a requirement, so no pipeline is refused for it.
    /// The shader itself must be correct at any size.
    /// SGL states it, and a compiler's cache holds the compiler's own result, so a cached shader never carries it.
    cc::optional<i32> preferred_subgroup_size;
    /// A compute shader whose compiler chose `workgroup_size`, as a bare SGL `@compute` has it: sg dispatches it by
    /// threads alone, writing the grid its threads past it return before, and refuses a dispatch by groups.
    /// SGL states it, and a compiler's cache holds the compiler's own result, so a cached shader never carries it.
    bool is_dispatched_by_threads = false;
    /// A polyfilled mesh stage's numbers, which both its halves carry alike: a primitive's corners, the primitives and
    /// the 32-bit words of one workgroup's slots (libs/graphics/shaped-graphics/docs/concepts/mesh-polyfill.md); all 0 for every other shader.
    /// SGL states them, and a compiler's cache holds the compiler's own result, so a cached shader never carries them.
    i32 mesh_polyfill_corners = 0;
    i32 mesh_polyfill_primitives = 0;
    i32 mesh_polyfill_words = 0;
    /// A polyfilled task stage's numbers, which its compute form and its mesh stage's compute half and scan carry
    /// alike: the 32-bit words of one task workgroup's record, and the mesh workgroups one launches at most, 0 where
    /// it states none; all 0 for every other shader.
    /// SGL states them, as it does the mesh stage's.
    i32 task_polyfill_record_words = 0;
    i32 task_polyfill_meshes = 0;
    /// A polyfilled tessellation's numbers, which its control stage's and its evaluation stage's halves carry alike
    /// (SGL's CHK-527): the domain's edges (2 isolines, 3, 4), the patch's control points, the 32-bit words a patch
    /// takes, the words a point takes, the partitioning (0 integer, 1 fractional even, 2 fractional odd), and D3D's
    /// output topology (0 `triangle_cw`, 1 `triangle_ccw`, 2 lines), and the control stage's largest factor; all 0 for
    /// every other shader.
    /// The vertex half carries `tessellation_vertex_words` alone.
    i32 tessellation_domain = 0;
    i32 tessellation_control_points = 0;
    i32 tessellation_patch_words = 0;
    i32 tessellation_vertex_words = 0;
    i32 tessellation_partitioning = 0;
    i32 tessellation_output = 0;
    i32 tessellation_max_factor = 0;

    compiler_info compiler;

    /// A fragment shader's render-target count, where its compiler reflects one; a pipeline must have as many.
    /// One past the highest target it writes, since targets may be sparse.
    cc::optional<i32> color_output_count;
    /// The target set a fragment shader writes, as a generated SGL render target names it; empty where there is none.
    /// A pipeline description that names no target set takes this one.
    /// slib sets it once a compile settles, so a cached shader never carries it.
    cc::string target_set;

    /// The features a device needs to run it, which pipeline creation refuses a device without.
    /// Empty is the portable baseline, and nullopt is unknown: a compiler that cannot tell, such as one reading HLSL, says so.
    /// An unknown shader is built as a portable one would be, and only the backend can still refuse it.
    /// SGL states it, and a compiler's cache holds the compiler's own result, so a cached shader never carries it.
    cc::optional<feature_set> required_features;

    /// What the code does to each binding, which barrier inference follows at dispatch.
    /// SGL states an `exact` one; a compiler's reflection gives a `reflected` one.
    /// Unknown (`none`) where nothing could tell, and then every writable view counts as written.
    shader_footprint footprint;

    /// Every print and assert the code writes into the context's debug buffer; empty for a shader that writes none.
    /// SGL states them when it compiled with diagnostics, and a shader that has any binds the buffer in sg's reserved slot.
    cc::vector<shader_debug_site> debug_sites;

    /// Where each line of a text format's code came from, so a backend that compiles the text names the source's line.
    /// SGL states it; empty for every other compiler, and for bytecode, whose compiler already reported its lines.
    cc::vector<shader_source_line> source_map;

    // Deferred: constant-buffer member layouts, root/push constants, content hash, I/O signatures.
};
