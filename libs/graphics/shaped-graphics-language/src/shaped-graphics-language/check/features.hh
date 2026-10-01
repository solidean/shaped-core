#pragma once

#include <clean-core/common/flags.hh>
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
    /// `half` and its vectors (CHK-347).
    shader_f16,
    /// `short`, `ushort` and their vectors (CHK-347), which WebGPU has on no device.
    shader_int16,
    /// The `subgroup_*` and `quad_*` operations (CHK-376).
    subgroups,
    /// A `@coherent` member (CHK-368), which WebGPU has on no device.
    device_coherence,
    /// An `@atomic` image (CHK-372), which WebGPU has on no device.
    image_atomics,
};

CC_FLAG_ENUM_INDEXED(sgl::check, feature, u16);

namespace sgl::check
{
using feature_set = cc::flags<feature>;

inline constexpr cc::string_view k_feature_names[] = {
    "binding_arrays",              //
    "extended_image_formats",      //
    "readwrite_image_formats",     //
    "multisampled_array_textures", //
    "ray_query",                   //
    "raytracing_pipeline",         //
    "primitive_index",             //
    "sample_rate_shading",         //
    "geometry_shader",             //
    "tessellation_shader",         //
    "shader_f16",                  //
    "shader_int16",                //
    "subgroups",                   //
    "device_coherence",            //
    "image_atomics",               //
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
