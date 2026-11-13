#pragma once

#include <clean-core/common/flags.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>

/// What a context can and cannot do, as one vocabulary rather than one spelling per question.
///
/// The set is deliberately small and coarse.
/// Every added granularity is a new way for a renderer to be non-portable without noticing: a caller that branches on
/// a fine-grained capability is a caller that behaves differently per backend, which is the thing sg exists to avoid.
/// So a `feature` earns its place only when a whole code path is present or absent, never to describe a difference of
/// degree.

/// When a stage is drawn as its polyfill rather than natively.
enum class sg::polyfill_use : sg::u8
{
    never,
    /// Where the device lacks the stage.
    where_missing,
    /// On every device, the stage's own included: what compares the two on one device.
    always,
};

/// How a polyfilled geometry stage draws (SGL's CHK-525).
enum class sg::geometry_polyfill_route : sg::u8
{
    /// A compute pass ahead of the draw writes each primitive's slots, and the draw reads them back; on every backend that
    /// draws the polyfill.
    compute,
    /// One mesh draw: the vertex and geometry stages as a mesh stage, a workgroup's lanes writing their slots of its
    /// output, with no buffer of the draw's own and no pass split; where the device has mesh shaders.
    mesh,
};

/// A capability a context either has or does not.
/// Absent means the code path does not exist on this backend or device — not that it is slow.
enum class sg::feature
{
    /// Inline ray tracing: a trace called from an ordinary stage (HLSL's `RayQuery`, MSL's `intersection_query`).
    /// Acceleration structures exist wherever this or `raytracing_pipeline` does.
    /// WebGPU has it through a software polyfill, which `context::implementation_of` reports as emulated.
    ray_query,

    /// The ray-tracing pipeline: raygen, miss, hit and callable shaders, the shader table and `dispatch_rays`.
    /// A device fact as much as a backend one, since an adapter may lack DXR / VK_KHR_ray_tracing_pipeline.
    raytracing_pipeline,

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

    /// The mesh stage exists, and the task stage that launches it, so a raster pipeline may draw from workgroups rather than vertices.
    /// D3D12 grants it with mesh shader tier 1, Vulkan with VK_EXT_mesh_shader's `meshShader` and `taskShader`.
    /// Metal grants it with its object and mesh stages, and WebGPU has neither.
    mesh_shader,

    /// A binding may be an array (`count > 1`), and so `staging_binding_group` and `bindless_array` work.
    /// WebGPU core has no binding arrays at all, so a bindless renderer asks once rather than finding out at layout creation.
    binding_arrays,

    /// An image may be `read_write` in any image format, not only r32float / r32uint / r32sint.
    /// WebGPU core allows read-write on those three alone, and its `texture-formats-tier2` lifts that; write-only and read-only work everywhere.
    readwrite_image_formats,

    /// A sampled texture of `r32_float`, `rg32_float` or `rgba32_float` may be filtered.
    /// Core WebGPU makes those three unfilterable and its `float32-filterable` lifts that, and Vulkan reports it per format.
    /// Without it, binding such a view to a `filterable_float` binding is refused on every backend.
    float32_filtering,

    /// An image may use a format outside `sg::is_portable_image_format`, such as `r8_unorm` or `rgb10a2_unorm`.
    /// `bgra8_unorm` is among them, and it is the one each API asks for on its own.
    /// WebGPU grants them with `texture-formats-tier1` and `bgra8unorm-storage`.
    /// Vulkan grants them with `shaderStorageImageExtendedFormats` plus `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT` on `B8G8R8A8_UNORM`.
    /// D3D12 grants them with a typed UAV on `DXGI_FORMAT_B8G8R8A8_UNORM`, the rest being required at feature level 11_0.
    extended_image_formats,

    /// A block-compressed texture may have a width or height that is no multiple of its block (4 for BC).
    /// WebGPU core refuses one unless the device has `texture-compression-unaligned`, and D3D12 reports it as an option.
    /// Where this is false, creating one is a refusal naming the size, which a loader of user textures can pad against.
    unaligned_block_compression,

    /// A texture may have a BC format, `bc1_rgba_unorm` through `bc7_rgba_unorm_srgb`, and be sampled and copied into.
    /// D3D12 requires it at feature level 11_0; Vulkan grants it with `textureCompressionBC`, which creation enables.
    /// Metal grants it on macOS GPUs that support BC compression, and WebGPU with `texture-compression-bc`.
    /// Where this is false, creating a BC texture is refused.
    texture_compression_bc,

    /// A texture binding may be a multisampled 2D array (`texture_view_dimension::tex_2d_ms_array`).
    /// WebGPU has no such binding at all, and it is also how a multisampled cube is sampled.
    multisampled_array_textures,

    /// A pixel shader may read which primitive it belongs to (SGL's `@primitive_id`).
    /// Vulkan gives it only with the `geometryShader` device feature, and WebGPU behind `primitive-index`.
    primitive_index,

    /// A pixel shader may run per sample: read `@sample_index`, or interpolate a member at each sample.
    /// Vulkan gives it only with the `sampleRateShading` device feature; D3D12, Metal and WebGPU always.
    sample_rate_shading,

    /// A raster pipeline may fill triangles as wireframe (`fill_mode::wireframe`).
    /// WebGPU has no wireframe fill at all, and Vulkan gives it only with the `fillModeNonSolid` device feature.
    wireframe_fill,

    /// A texture or a raster pipeline may use `pixel_format::depth32_float_stencil8`, sg's one format with a stencil aspect.
    /// WebGPU has it only with the optional `depth32float-stencil8` feature, and Vulkan asks the device per format.
    depth32_float_stencil8,

    /// A shader may compute and store 16-bit floats (SGL's `half`), in registers and in buffers.
    /// D3D12 grants it with `Native16BitShaderOpsSupported`, Vulkan with `shaderFloat16` plus 16-bit buffer storage.
    /// Where Vulkan grants `subgroups` too it also needs `shaderSubgroupExtendedTypes`, since a subgroup operation takes 16-bit values.
    /// It promises no 16-bit push constant on Vulkan, and SGL refuses a 16-bit member in an `@inline` binding.
    /// WebGPU grants it with the optional `shader-f16` feature, and Metal always.
    shader_f16,

    /// A shader may compute and store 16-bit integers (SGL's `short` and `ushort`), in registers and in buffers.
    /// D3D12 grants it with `Native16BitShaderOpsSupported`, Vulkan with `shaderInt16` plus 16-bit buffer storage.
    /// Where Vulkan grants `subgroups` too it also needs `shaderSubgroupExtendedTypes`, since a subgroup operation takes 16-bit values.
    /// It promises no 16-bit push constant on Vulkan, and SGL refuses a 16-bit member in an `@inline` binding.
    /// WebGPU has no 16-bit integers at all, and Metal always has them.
    shader_int16,

    /// A compute or pixel shader may use subgroup operations: vote, ballot, reductions, prefix sums, shuffles and quads.
    /// D3D12 grants them with `WaveOps`, Vulkan when its subgroup properties cover those stages and operations.
    /// Vulkan also needs linear compute derivatives, the groups DXC writes a compute stage's quad operations in.
    /// WebGPU grants them with the optional `subgroups` feature, and Metal always.
    subgroups,

    /// A buffer or image member may be coherent across the whole device, so one workgroup sees another's writes.
    /// D3D12 (`globallycoherent`) and Vulkan (`Coherent`) always have it, Metal from MSL 3.2's `coherent(device)`.
    /// WebGPU has no such qualifier.
    device_coherence,

    /// A shader may update a 32-bit integer image atomically.
    /// D3D12 requires typed UAV atomics on r32 formats and Vulkan requires storage-image atomics on them, Metal has them from Apple7.
    /// WebGPU has no image atomics.
    image_atomics,

    /// A vertex shader may read the draw's first instance on its own (SGL's `@first_instance`), the same for the whole draw.
    /// It is how one draw of an indirect batch learns which it is, uniformly.
    /// D3D12 has it from shader model 6.8, Vulkan from `shaderDrawParameters`, which creation requires, and Metal always.
    /// WGSL has no builtin for it.
    first_instance,

    /// An indirect draw's `first_instance` may be other than 0.
    /// Without it such a record is invalid on vulkan and draws nothing on WebGPU, with no error on either.
    /// D3D12 and Metal always have it; vulkan needs `drawIndirectFirstInstance` and WebGPU `indirect-first-instance`.
    indirect_first_instance,

    /// A pixel shader may tell a helper invocation from a real one, which SGL's strong compare-exchange needs there (EMIT-163).
    /// D3D12 has HLSL's `IsHelperLane()` from SM 6.6, Vulkan has the core `HelperInvocation` built-in, Metal has `simd_is_helper_thread()`.
    /// D3D12's and Vulkan's compare-exchange is strong already, so their strong form needs no loop and asks no test.
    /// WGSL has no such test.
    helper_lane_query,

    /// A triangle geometry may carry an opacity micromap (`cmd.raytracing.build_micromap`), which traversal decides most candidates by.
    /// Vulkan grants it with `VK_EXT_opacity_micromap`, D3D12 from DXR 1.2 in the Agility SDK; metal and WebGPU have no equivalent.
    /// A shader traces the same way either way, so none names it; SGL states it for a dx12 ray query, which then compiles at SM 6.9.
    opacity_micromap,

    /// A vertex shader may send its primitive to one layer of a layered render target (SGL's `@layer`), so one draw
    /// fills every layer of an array, such as both eyes of a stereo frame.
    /// D3D12 grants it with `VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation`, Vulkan with
    /// `shaderOutputLayer`, and Metal on every Apple GPU its Metal 4 floor admits; WebGPU has no such output.
    vertex_layer_output,

    /// A shader may add to a 32-bit float atomically in one instruction, in a buffer and in workgroup memory.
    /// Vulkan grants it with `VK_EXT_shader_atomic_float`'s `shaderBufferFloat32AtomicAdd` and `shaderSharedFloat32AtomicAdd`,
    /// and Metal on every Apple GPU its Metal 4 floor admits; D3D12 and WebGPU have none.
    /// SGL's `atomic[float].add` is a compare-exchange loop without it, and the native add where a context has it: slib
    /// hands an SGL compile every feature its context has, which SGL's `has` also answers.
    float_atomic_add,

    /// A ray-generation shader may reorder its invocations by a coherence hint, so threads that will run alike run together.
    /// D3D12 grants it from Shader Model 6.9 beside ray-tracing pipelines, which only the Agility SDK's runtime has.
    /// Vulkan grants it through `VK_EXT_ray_tracing_invocation_reorder` on a device that reorders rather than only accepting the calls.
    /// Metal and WebGPU have none.
    /// SGL's `maybe_reorder` is a hint: without the feature it does nothing, so it is what `has` asks.
    shader_execution_reordering,

    /// A shader may ask its 32-bit floats to keep NaNs, infinities and signed zeros, as SGL's `@float(.specials)` does.
    /// Vulkan grants it with `shaderSignedZeroInfNanPreserveFloat32`, which SPIR-V's `SignedZeroInfNanPreserve` needs.
    /// D3D12 and Metal keep them under a compile flag, `-Gis` and safe math, so they grant it; WebGPU keeps none.
    float_specials,

    /// A shader may ask its 32-bit floats to keep denormals rather than flush them, as SGL's `@float(.ieee)` does.
    /// Vulkan grants it with `shaderDenormPreserveFloat32`, which SPIR-V's `DenormPreserve` needs.
    /// D3D12 grants it, Shader Model 6.2 on, through DXC's `-denorm preserve`; Metal is not known to keep them, and
    /// WebGPU keeps none.
    float_denormals,

    /// One draw renders several views, view `i` into layer `i` of every target, as a pipeline's `view_count` asks and
    /// SGL's `@view_index` reads.
    /// What a stereo frame wants on a tiled GPU, where the driver bins both views together: layered rendering through
    /// `vertex_layer_output` rasterises them separately.
    /// D3D12 grants it with a `ViewInstancingTier`, Vulkan with `multiview`, Metal with vertex amplification of two views,
    /// and WebGPU has no such thing.
    view_instancing,

    /// Not a capability: this device's driver may write nothing for a draw of several views whose pixel stage ignores the view.
    /// SGL's `keep_view` reads it where this is set, and is free everywhere else.
    /// See docs/bugs-external/intel-d3d12-view-instancing-draws-nothing.
    pixel_view_read_workaround,

    /// A vertex, geometry or tessellation stage may write a storage buffer, which SGL's `print` and `assert` need there.
    /// D3D12 and Metal grant it on every device, Vulkan with `vertexPipelineStoresAndAtomics`, and WebGPU on none.
    vertex_pipeline_stores,

    /// A shader may compute and store 64-bit integers (SGL's `long` and `ulong`), in registers and in buffers.
    /// D3D12 grants it with `Int64ShaderOps`, Vulkan with `shaderInt64`, and Metal always; WebGPU has none.
    shader_int64,

    /// A shader may compute and store 64-bit floats (SGL's `double`), in registers and in buffers.
    /// D3D12 grants it with `DoublePrecisionFloatShaderOps`, Vulkan with `shaderFloat64`; Metal and WebGPU have none.
    shader_f64,

    /// A shader may update a 64-bit integer of a buffer atomically (SGL's `atomic[long]` in a `mut buffer`).
    /// D3D12 grants it with `Int64ShaderOps`, since Shader Model 6.6 makes raw and structured buffers' 64-bit atomics
    /// part of it; Vulkan grants it with `shaderBufferInt64Atomics`.
    /// Workgroup memory's are optional on both, and are `shader_shared_int64_atomics`.
    /// Metal's 64-bit atomics are a ulong's min and max alone, and WebGPU has none.
    shader_int64_atomics,

    /// A binding of `count == 0`, an array whose length each binding group decides by the views it is given.
    /// It is the last binding of its layout, and of its kind's table, which is where both APIs allow one.
    /// D3D12 grants it at resource binding tier 3, Vulkan with `descriptorBindingVariableDescriptorCount`.
    /// Metal reaches it through a table each group sizes behind its argument buffer, and WebGPU has no binding arrays.
    unbounded_binding_arrays,

    /// A shader may update a 64-bit integer of workgroup memory atomically (SGL's `atomic[long]` in a `@workgroup` binding).
    /// D3D12 grants it with `AtomicInt64OnGroupSharedSupported`, Vulkan with `shaderSharedInt64Atomics`; both make it optional.
    shader_shared_int64_atomics,

    /// A shader may take the minimum or the maximum of a buffer's 64-bit unsigned integer atomically, and do nothing else to it.
    /// Every device with `shader_int64_atomics` has it; Metal grants it alone, on the Apple GPUs that have 64-bit atomics.
    shader_int64_min_max_atomics,

    /// A TLAS instance (`tlas_instance::transform_at_end`) and a BLAS's vertices (`blas_triangles::later_poses`) may move
    /// over the frame, and a trace places them at its ray's time.
    /// **Emulated wherever it is offered**: webgpu through its polyfill, dx12 and vulkan through a package's SOFTWARE_TRACE.
    /// A native trace sees none of it, since DXR has no motion and vulkan's VK_NV_ray_tracing_motion_blur reaches no ray query.
    /// metal offers it natively to a pipeline's trace, and through the software walk to an inline one, whose query has no motion.
    ray_tracing_motion_blur,

    /// A tlas may be a `partitioned_tlas`: created with fixed capacities, and updated in place by `cmd.raytracing.update`.
    /// Vulkan grants it with `VK_NV_partitioned_acceleration_structure`, and traces it by its address.
    /// D3D12 grants it only on the preview Agility SDK, with `ClustersAndPTLASSupported`; metal and WebGPU have none.
    partitioned_tlas,

    /// A blas may be built from clusters: `cmd.raytracing.build_clusters`, then `build_cluster_blas` over ranges of them.
    /// Vulkan grants it with `VK_NV_cluster_acceleration_structure`, and a shader reads a hit's cluster as `cluster_id`.
    /// D3D12 grants it only on the preview Agility SDK, with `ClustersAndPTLASSupported`; metal and WebGPU have none.
    clustered_geometry,
};

CC_FLAG_ENUM_INDEXED(sg, feature, cc::u64);

namespace sg
{
/// A set of features: what a shader needs of a device, or what a device has.
using feature_set = cc::flags<feature>;

/// Every feature, in the enum's order.
inline constexpr feature k_all_features[] = {
    feature::ray_query,
    feature::raytracing_pipeline,
    feature::timestamp_query,
    feature::headless_present,
    feature::geometry_shader,
    feature::tessellation_shader,
    feature::mesh_shader,
    feature::binding_arrays,
    feature::readwrite_image_formats,
    feature::float32_filtering,
    feature::extended_image_formats,
    feature::unaligned_block_compression,
    feature::texture_compression_bc,
    feature::multisampled_array_textures,
    feature::primitive_index,
    feature::sample_rate_shading,
    feature::wireframe_fill,
    feature::depth32_float_stencil8,
    feature::shader_f16,
    feature::shader_int16,
    feature::subgroups,
    feature::device_coherence,
    feature::image_atomics,
    feature::first_instance,
    feature::indirect_first_instance,
    feature::helper_lane_query,
    feature::opacity_micromap,
    feature::vertex_layer_output,
    feature::float_atomic_add,
    feature::shader_execution_reordering,
    feature::float_specials,
    feature::float_denormals,
    feature::view_instancing,
    feature::pixel_view_read_workaround,
    feature::vertex_pipeline_stores,
    feature::shader_int64,
    feature::shader_f64,
    feature::shader_int64_atomics,
    feature::unbounded_binding_arrays,
    feature::shader_shared_int64_atomics,
    feature::shader_int64_min_max_atomics,
    feature::ray_tracing_motion_blur,
    feature::partitioned_tlas,
    feature::clustered_geometry,
};
static_assert(isize(sizeof(k_all_features) / sizeof(k_all_features[0])) == isize(feature::clustered_geometry) + 1,
              "k_all_features lists every feature");

/// The enumerator's name, `ray_query`, which is also what SGL's `require` spells it as.
[[nodiscard]] constexpr cc::string_view to_string(feature f)
{
    switch (f)
    {
    case feature::ray_query:
        return "ray_query";
    case feature::raytracing_pipeline:
        return "raytracing_pipeline";
    case feature::timestamp_query:
        return "timestamp_query";
    case feature::headless_present:
        return "headless_present";
    case feature::geometry_shader:
        return "geometry_shader";
    case feature::tessellation_shader:
        return "tessellation_shader";
    case feature::mesh_shader:
        return "mesh_shader";
    case feature::binding_arrays:
        return "binding_arrays";
    case feature::readwrite_image_formats:
        return "readwrite_image_formats";
    case feature::float32_filtering:
        return "float32_filtering";
    case feature::extended_image_formats:
        return "extended_image_formats";
    case feature::unaligned_block_compression:
        return "unaligned_block_compression";
    case feature::texture_compression_bc:
        return "texture_compression_bc";
    case feature::multisampled_array_textures:
        return "multisampled_array_textures";
    case feature::primitive_index:
        return "primitive_index";
    case feature::sample_rate_shading:
        return "sample_rate_shading";
    case feature::wireframe_fill:
        return "wireframe_fill";
    case feature::depth32_float_stencil8:
        return "depth32_float_stencil8";
    case feature::shader_f16:
        return "shader_f16";
    case feature::shader_int16:
        return "shader_int16";
    case feature::subgroups:
        return "subgroups";
    case feature::device_coherence:
        return "device_coherence";
    case feature::image_atomics:
        return "image_atomics";
    case feature::first_instance:
        return "first_instance";
    case feature::indirect_first_instance:
        return "indirect_first_instance";
    case feature::helper_lane_query:
        return "helper_lane_query";
    case feature::opacity_micromap:
        return "opacity_micromap";
    case feature::vertex_layer_output:
        return "vertex_layer_output";
    case feature::float_atomic_add:
        return "float_atomic_add";
    case feature::shader_execution_reordering:
        return "shader_execution_reordering";
    case feature::float_specials:
        return "float_specials";
    case feature::float_denormals:
        return "float_denormals";
    case feature::view_instancing:
        return "view_instancing";
    case feature::pixel_view_read_workaround:
        return "pixel_view_read_workaround";
    case feature::vertex_pipeline_stores:
        return "vertex_pipeline_stores";
    case feature::shader_int64:
        return "shader_int64";
    case feature::shader_f64:
        return "shader_f64";
    case feature::shader_int64_atomics:
        return "shader_int64_atomics";
    case feature::unbounded_binding_arrays:
        return "unbounded_binding_arrays";
    case feature::shader_shared_int64_atomics:
        return "shader_shared_int64_atomics";
    case feature::shader_int64_min_max_atomics:
        return "shader_int64_min_max_atomics";
    case feature::ray_tracing_motion_blur:
        return "ray_tracing_motion_blur";
    case feature::partitioned_tlas:
        return "partitioned_tlas";
    case feature::clustered_geometry:
        return "clustered_geometry";
    }
    return "";
}

/// The feature named `name` as `to_string` spells it; nullopt for a name that is none.
[[nodiscard]] constexpr cc::optional<feature> feature_from_string(cc::string_view name)
{
    for (auto const f : k_all_features)
        if (to_string(f) == name)
            return f;
    return {};
}
} // namespace sg

/// How a context provides a feature, or that it has none.
/// A caller that picks an algorithm by cost asks this; a shader never does, since both run the same source.
enum class sg::feature_implementation
{
    /// The context lacks the feature: `supports` answers no.
    absent,

    /// The device does it.
    native,

    /// sg does it in software on top of the device, correctly and more slowly: webgpu's `ray_query`.
    emulated,
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
