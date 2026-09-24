#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/fwd.hh>
#include <shaped-graphics-language/interpret/scalar.hh>

/// What the texture, image and sampler types of a binding are made of: their shapes and an image's storage formats.
/// One table each, read by the check pass, every emitter and the host description alike (the spec's bindings file).

/// The shape of a texture or an image, which is sg's `texture_view_dimension` member for member.
enum class sgl::check::texture_shape : sgl::u8
{
    d1,
    d1_array,
    d2,
    d2_array,
    d2_ms,
    d2_ms_array,
    d3,
    cube,
    cube_array,
};

/// One shape, and how each family spells it.
struct sgl::check::shape_info
{
    texture_shape shape;
    /// The sampled texture's type name: `texture2d_array`.
    cc::string_view texture;
    /// The depth texture's type name, empty where no target has one: `texture2d_array_depth`.
    cc::string_view depth;
    /// The image's type name, empty where no target has one: `image2d_array`.
    cc::string_view image;
    /// sg's `texture_view_dimension` member, which the host description names.
    cc::string_view sg_name;
    /// Needs a feature some backend lacks, so it is refused until a function can opt into one (bindings, "Features").
    cc::string_view feature;
};

namespace sgl::check
{

inline constexpr shape_info k_shapes[] = {
    {texture_shape::d1, "texture1d", "", "image1d", "tex_1d", ""},
    {texture_shape::d1_array, "texture1d_array", "", "image1d_array", "tex_1d_array", ""},
    {texture_shape::d2, "texture2d", "texture2d_depth", "image2d", "tex_2d", ""},
    {texture_shape::d2_array, "texture2d_array", "texture2d_array_depth", "image2d_array", "tex_2d_array", ""},
    {texture_shape::d2_ms, "texture2d_ms", "texture2d_ms_depth", "", "tex_2d_ms", ""},
    {texture_shape::d2_ms_array, "texture2d_ms_array", "", "", "tex_2d_ms_array", "multisampled arrays"},
    {texture_shape::d3, "texture3d", "", "image3d", "tex_3d", ""},
    {texture_shape::cube, "texture_cube", "texture_cube_depth", "", "cube", ""},
    {texture_shape::cube_array, "texture_cube_array", "texture_cube_array_depth", "", "cube_array", ""},
};

[[nodiscard]] constexpr shape_info const& info_of(texture_shape s)
{
    return k_shapes[isize(s)];
}

} // namespace sgl::check

/// One storage format an image may take, named as `sg::pixel_format` names it.
struct sgl::check::storage_format_info
{
    cc::string_view name;
    /// WGSL's spelling, which drops the underscore and calls one float format `ufloat`.
    cc::string_view wgsl;
    /// What DXC's `[[vk::image_format]]` takes; empty where SPIR-V has no such format.
    cc::string_view vulkan;
    /// How many channels a texel has, which is the width of the vector a load gives.
    i32 channels;
    /// What a channel reads as in the shader: a float for every normalized and float format.
    value_kind component;
    /// Core WebGPU stores it without a feature.
    bool is_portable;
    /// Core WebGPU also reads and writes it in one shader, which only the three `r32` formats are.
    bool is_readwrite_portable;
};

namespace sgl::check
{

inline constexpr auto k_float = value_kind::scalar_float;
inline constexpr auto k_sint = value_kind::scalar_int;
inline constexpr auto k_uint = value_kind::scalar_uint;

inline constexpr storage_format_info k_storage_formats[] = {
    {"r8_unorm", "r8unorm", "r8", 1, k_float, false, false},
    {"r8_snorm", "r8snorm", "r8snorm", 1, k_float, false, false},
    {"r8_uint", "r8uint", "r8ui", 1, k_uint, false, false},
    {"r8_sint", "r8sint", "r8i", 1, k_sint, false, false},
    {"rg8_unorm", "rg8unorm", "rg8", 2, k_float, false, false},
    {"rg8_snorm", "rg8snorm", "rg8snorm", 2, k_float, false, false},
    {"rg8_uint", "rg8uint", "rg8ui", 2, k_uint, false, false},
    {"rg8_sint", "rg8sint", "rg8i", 2, k_sint, false, false},
    {"rgba8_unorm", "rgba8unorm", "rgba8", 4, k_float, true, false},
    {"rgba8_snorm", "rgba8snorm", "rgba8snorm", 4, k_float, true, false},
    {"rgba8_uint", "rgba8uint", "rgba8ui", 4, k_uint, true, false},
    {"rgba8_sint", "rgba8sint", "rgba8i", 4, k_sint, true, false},
    {"bgra8_unorm", "bgra8unorm", "", 4, k_float, false, false},
    {"r16_float", "r16float", "r16f", 1, k_float, false, false},
    {"r16_uint", "r16uint", "r16ui", 1, k_uint, false, false},
    {"r16_sint", "r16sint", "r16i", 1, k_sint, false, false},
    {"rg16_float", "rg16float", "rg16f", 2, k_float, false, false},
    {"rg16_uint", "rg16uint", "rg16ui", 2, k_uint, false, false},
    {"rg16_sint", "rg16sint", "rg16i", 2, k_sint, false, false},
    {"rgba16_float", "rgba16float", "rgba16f", 4, k_float, true, false},
    {"rgba16_uint", "rgba16uint", "rgba16ui", 4, k_uint, true, false},
    {"rgba16_sint", "rgba16sint", "rgba16i", 4, k_sint, true, false},
    {"r32_float", "r32float", "r32f", 1, k_float, true, true},
    {"r32_uint", "r32uint", "r32ui", 1, k_uint, true, true},
    {"r32_sint", "r32sint", "r32i", 1, k_sint, true, true},
    {"rg32_float", "rg32float", "rg32f", 2, k_float, true, false},
    {"rg32_uint", "rg32uint", "rg32ui", 2, k_uint, true, false},
    {"rg32_sint", "rg32sint", "rg32i", 2, k_sint, true, false},
    {"rgba32_float", "rgba32float", "rgba32f", 4, k_float, true, false},
    {"rgba32_uint", "rgba32uint", "rgba32ui", 4, k_uint, true, false},
    {"rgba32_sint", "rgba32sint", "rgba32i", 4, k_sint, true, false},
    {"rgb10a2_unorm", "rgb10a2unorm", "rgb10a2", 4, k_float, false, false},
    {"rg11b10_float", "rg11b10ufloat", "r11g11b10f", 3, k_float, false, false},
};

/// The type a texel of `format` loads as and stores from: `float4` for `rgba8_unorm`, `float` for `r32_float`.
[[nodiscard]] cc::string texel_name_of(i32 format);

/// A position in `k_storage_formats`, or -1 for a name that is no storage format.
[[nodiscard]] constexpr i32 find_storage_format(cc::string_view name)
{
    for (auto i = 0; i < i32(sizeof(k_storage_formats) / sizeof(k_storage_formats[0])); ++i)
        if (k_storage_formats[i].name == name)
            return i;
    return -1;
}
} // namespace sgl::check

namespace sgl::check
{
/// The values of a sampler's enum settings, in the order `sg::sampler_filter`, `sg::sampler_address_mode` and
/// `sg::compare_op` declare them, so a position here is that enum's value.
inline constexpr cc::string_view k_sampler_filters[] = {"nearest", "linear"};
inline constexpr cc::string_view k_sampler_addresses[] = {"repeat", "mirror_repeat", "clamp_edge"};
inline constexpr cc::string_view k_compare_ops[]
    = {"never", "less", "equal", "less_equal", "greater", "not_equal", "greater_equal", "always"};
} // namespace sgl::check
