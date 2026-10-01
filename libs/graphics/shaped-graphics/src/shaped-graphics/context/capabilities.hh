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
    /// WebGPU grants it with the optional `shader-f16` feature, and Metal always.
    shader_f16,

    /// A shader may compute and store 16-bit integers (SGL's `short` and `ushort`), in registers and in buffers.
    /// D3D12 grants it with `Native16BitShaderOpsSupported`, Vulkan with `shaderInt16` plus 16-bit buffer storage.
    /// WebGPU has no 16-bit integers at all, and Metal always has them.
    shader_int16,

    /// A compute or pixel shader may use subgroup operations: vote, ballot, reductions, prefix sums, shuffles and quads.
    /// D3D12 grants them with `WaveOps`, Vulkan when its subgroup properties cover those stages and operations.
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
};

CC_FLAG_ENUM_INDEXED(sg, feature, cc::u32);

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
    feature::binding_arrays,
    feature::readwrite_image_formats,
    feature::float32_filtering,
    feature::extended_image_formats,
    feature::unaligned_block_compression,
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
};
static_assert(isize(sizeof(k_all_features) / sizeof(k_all_features[0])) == isize(feature::image_atomics) + 1,
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
