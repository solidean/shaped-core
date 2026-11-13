#pragma once

#include <clean-core/common/flags.hh>
#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/fwd.hh>

/// The features a `require` names: each one is the `sg::feature` of the same name, so the host asks the device exactly
/// what the shader needed (the spec's checking file, "Features").
/// SGL does not link sg, which is why the names are repeated here; sg's `context/capabilities.hh` is what they follow.
/// Only the features a shader can use are here, and an sg feature of the host alone is no name a `require` knows.

enum class sgl::check::feature : sgl::u8
{
    binding_arrays,
    extended_image_formats,
    readwrite_image_formats,
    multisampled_array_textures,
    /// A trace from an ordinary stage; WebGPU has it through the polyfill.
    ray_query,
    /// The ray-tracing stages, and a trace that invokes them.
    raytracing_pipeline,
    /// `@primitive_id` in the pixel stage, which vulkan gives only with the geometry stage and WGSL behind an extension.
    primitive_index,
    /// `@sample_index` and per-sample interpolation, which vulkan gives only with `sampleRateShading`.
    sample_rate_shading,
    geometry_shader,
    tessellation_shader,
    /// The mesh stage (CHK-480), which WebGPU has on no device.
    mesh_shader,
    /// `half` and its vectors (CHK-382).
    shader_f16,
    /// `short`, `ushort` and their vectors (CHK-382), which WebGPU has on no device.
    shader_int16,
    /// The `subgroup_*` and `quad_*` operations (CHK-376).
    subgroups,
    /// A `@coherent` member (CHK-368), which WebGPU has on no device.
    device_coherence,
    /// An `@atomic` image (CHK-372), which WebGPU has on no device.
    image_atomics,
    /// `@first_instance` in the vertex stage, which WGSL has no builtin for.
    first_instance,
    /// A pixel stage's `compare_exchange_strong` (EMIT-163), whose loop must tell a helper invocation; WGSL cannot.
    helper_lane_query,
    /// A `@layer` member (CHK-419): the vertex stage picks the render target's layer, which WebGPU has on no device.
    vertex_layer_output,
    /// A float added atomically in one instruction, which vulkan and metal write where the device has it.
    /// `atomic[float]` runs on every device without it, so it is what `has` asks rather than what a `require` needs.
    float_atomic_add,
    /// A ray-generation stage reorders its invocations by a hint, which dx12 on Shader Model 6.9 and vulkan write.
    /// `maybe_reorder` does nothing without it, so it is what `has` asks rather than what a `require` needs.
    shader_execution_reordering,
    /// A ray query that sees the opacity micromaps its BLASes hold, which only Shader Model 6.9 writes.
    /// No program names it: an HLSL compile for a device that has micromaps states it for every ray query (EMIT-184).
    opacity_micromap,
    /// 32-bit floats that keep NaNs, infinities and signed zeros, which vulkan asks of the device (EMIT-185).
    /// No program names it: a vulkan text needs it for an entry point at `@float(.specials)` or above.
    float_specials,
    /// 32-bit floats that keep denormals, which vulkan asks of the device for an entry point at `@float(.ieee)`.
    float_denormals,
    /// `@view_index` (CHK-437): one draw renders several views, which WebGPU has on no device.
    view_instancing,
    /// No program names it: `keep_view` asks for it with `has`, on a device whose driver needs the pixel stage to read the view.
    pixel_view_read_workaround,
    /// A vertex, geometry or tessellation stage writes a storage buffer, which vulkan asks of the device (EMIT-183).
    /// No program names it: a vulkan text writes a `print` from those stages where the device has it, and drops one elsewhere.
    vertex_pipeline_stores,
    /// `long`, `ulong` and their vectors (CHK-451), which WebGPU has on no device.
    shader_int64,
    /// `double` and its vectors (CHK-451), which WebGPU and Metal have on no device.
    shader_f64,
    /// `atomic[long]` and `atomic[ulong]` in a buffer (CHK-296), which WebGPU and Metal have on no device.
    shader_int64_atomics,
    /// A binding array `T[]` whose length the host binds (CHK-490), which WebGPU and Metal have on no device yet.
    unbounded_binding_arrays,
    /// `atomic[long]` and `atomic[ulong]` in workgroup memory (CHK-510), which both APIs make optional.
    shader_shared_int64_atomics,
    /// `atomic[ulong]` in a buffer, its `min` and `max` alone (CHK-511); `shader_int64_atomics` grants it too.
    /// It is all of Metal's 64-bit atomics, on the GPUs that have any.
    shader_int64_min_max_atomics,
    /// A hit's cluster, which `cluster_id` reads where the device builds a BLAS from clusters, and is -1 elsewhere.
    /// No program names it: the prelude asks it with `has`, so a device without it needs no capability of it.
    clustered_geometry,
};

CC_FLAG_ENUM_INDEXED(sgl::check, feature, u64);

namespace sgl::check
{
using feature_set = cc::flags<feature>;

inline constexpr cc::string_view k_feature_names[] = {
    "binding_arrays",               //
    "extended_image_formats",       //
    "readwrite_image_formats",      //
    "multisampled_array_textures",  //
    "ray_query",                    //
    "raytracing_pipeline",          //
    "primitive_index",              //
    "sample_rate_shading",          //
    "geometry_shader",              //
    "tessellation_shader",          //
    "mesh_shader",                  //
    "shader_f16",                   //
    "shader_int16",                 //
    "subgroups",                    //
    "device_coherence",             //
    "image_atomics",                //
    "first_instance",               //
    "helper_lane_query",            //
    "vertex_layer_output",          //
    "float_atomic_add",             //
    "shader_execution_reordering",  //
    "opacity_micromap",             //
    "float_specials",               //
    "float_denormals",              //
    "view_instancing",              //
    "pixel_view_read_workaround",   //
    "vertex_pipeline_stores",       //
    "shader_int64",                 //
    "shader_f64",                   //
    "shader_int64_atomics",         //
    "unbounded_binding_arrays",     //
    "shader_shared_int64_atomics",  //
    "shader_int64_min_max_atomics", //
    "clustered_geometry",           //
};
inline constexpr isize k_feature_count = isize(sizeof(k_feature_names) / sizeof(k_feature_names[0]));

[[nodiscard]] constexpr cc::string_view name_of(feature f)
{
    return k_feature_names[isize(f)];
}

/// The feature `name` names, or -1 for a name that is none.
[[nodiscard]] constexpr i32 find_feature(cc::string_view name)
{
    for (auto i = isize(0); i < k_feature_count; ++i)
        if (k_feature_names[i] == name)
            return i32(i);
    return -1;
}
} // namespace sgl::check

/// A DXIL shader model, which a dx12 host compiles SGL's text at and states for SGL's own DXIL.
/// 6.6 is the floor: a compile that asks no fill past it writes nothing for dx12 that needs more (EMIT-262).
struct sgl::check::dxil_shader_model
{
    i32 major = 6;
    i32 minor = 6;

    [[nodiscard]] constexpr bool operator==(dxil_shader_model const&) const = default;
    [[nodiscard]] constexpr bool operator<(dxil_shader_model const& rhs) const
    {
        return major < rhs.major || (major == rhs.major && minor < rhs.minor);
    }
};

/// How a compile writes a granted feature or a core construct for its target (EMIT-263).
/// `emit::fills_of` says which ones a target offers, most demanding first.
enum class sgl::check::fill : sgl::u8
{
    /// The target's own form, at the shader model `dxil_shader_model_of` gives: SER's `HitObject` is 6.9 on dx12.
    native,
    /// D3D12 on NVIDIA through NVAPI's shader extensions, which `nvHLSLExtns.h` declares; the host enables them.
    /// `shader_execution_reordering` is `NvHitObject` and `NvReorderThread`, and `opacity_micromap` writes nothing.
    nvapi,
    /// The draw's start handed over in a root constant, which no compile writes yet (EMIT-128).
    root_constant,
    /// The construct left out: no `WaveSize` at all (EMIT-148).
    none,
};

/// A construct every device runs whose target forms differ by shader model, which a compile fills like a feature.
enum class sgl::check::core_construct : sgl::u8
{
    /// Where a draw started, which `@vertex_index`, `@instance_index` and `@first_instance` read (EMIT-128).
    draw_start,
    /// A compute entry point's `@preferred_subgroup_size` stated in the text (EMIT-148).
    wave_size,
};

/// The fill a compile asks for one granted feature; a granted feature it names none for is `native`.
struct sgl::check::feature_fill
{
    feature f = feature::binding_arrays;
    fill how = fill::native;

    [[nodiscard]] constexpr bool operator==(feature_fill const&) const = default;
};

/// The fill a compile asks for each core construct; `native` for both is what every compile wrote before fills existed.
struct sgl::check::core_fills
{
    /// `native` or `root_constant`.
    fill draw_start = fill::native;
    /// `native` or `none`.
    fill wave_size = fill::native;

    [[nodiscard]] constexpr bool operator==(core_fills const&) const = default;
};

namespace sgl::check
{
inline constexpr cc::string_view k_fill_names[] = {"native", "nvapi", "root_constant", "none"};
inline constexpr cc::string_view k_core_construct_names[] = {"draw_start", "wave_size"};

[[nodiscard]] constexpr cc::string_view name_of(fill f)
{
    return k_fill_names[isize(f)];
}
[[nodiscard]] constexpr cc::string_view name_of(core_construct c)
{
    return k_core_construct_names[isize(c)];
}

/// The DXIL shader model a dx12 output needs where it writes `f` as `how` (EMIT-262).
/// 6.6 for every feature whose form needs no more, and for every fill but `native`.
[[nodiscard]] constexpr dxil_shader_model dxil_shader_model_of(feature f, fill how = fill::native)
{
    if (how != fill::native)
        return {};
    switch (f)
    {
    case feature::shader_execution_reordering:
    case feature::opacity_micromap:
        return {.major = 6, .minor = 9};
    case feature::clustered_geometry:
        return {.major = 6, .minor = 10};
    default:
        return {};
    }
}

/// The DXIL shader model a dx12 output needs where it writes `c` as `how`: 6.8 for either native form, 6.6 otherwise.
[[nodiscard]] constexpr dxil_shader_model dxil_shader_model_of(core_construct c, fill how)
{
    (void)c;
    return how == fill::native ? dxil_shader_model{.major = 6, .minor = 8} : dxil_shader_model{};
}

/// The register and space of the counter UAV through which an `nvapi` fill talks to NVIDIA's driver, `u0, space13`.
/// The text defines `NV_SHADER_EXTN_SLOT` and `NV_SHADER_EXTN_REGISTER_SPACE` to it, and the host enables that slot.
inline constexpr i32 k_nvapi_register = 0;
inline constexpr i32 k_nvapi_space = 13;

/// The fill `fills` asks for `f`, `native` where it names none.
[[nodiscard]] constexpr fill fill_of(cc::span<feature_fill const> fills, feature f)
{
    for (auto const& entry : fills)
        if (entry.f == f)
            return entry.how;
    return fill::native;
}
} // namespace sgl::check

/// The API a compile writes text for, which `target_api` reads once inlined (CHK-426).
/// `none` where no text is written, as in a test or a description.
/// The prelude's `graphics_api` enum lists these cases in this order.
enum class sgl::check::graphics_api : sgl::u8
{
    none,
    dx12,
    vulkan,
    metal,
    webgpu,
};

/// The language a compile writes, which `target_language` reads once inlined (CHK-426).
/// `sgl` where no text is written, since the interpreter runs SGL itself.
/// The prelude's `shading_language` enum lists these cases in this order.
enum class sgl::check::shading_language : sgl::u8
{
    sgl,
    hlsl,
    wgsl,
    msl,
};

namespace sgl::check
{
inline constexpr cc::string_view k_graphics_api_names[] = {"none", "dx12", "vulkan", "metal", "webgpu"};
inline constexpr cc::string_view k_shading_language_names[] = {"sgl", "hlsl", "wgsl", "msl"};
} // namespace sgl::check

/// What one compile writes for, which the check pass substitutes where an inlined body reads it (CHK-426).
/// The default is no target and no device, which is what a test runs as.
struct sgl::check::compile_target
{
    graphics_api api = graphics_api::none;
    shading_language language = shading_language::sgl;
    /// What the device has, which `has` answers; a feature outside it is one the device lacks.
    feature_set device_features;

    [[nodiscard]] bool operator==(compile_target const&) const = default;
};
