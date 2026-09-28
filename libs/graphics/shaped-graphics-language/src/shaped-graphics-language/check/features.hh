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
    raytracing,
};

CC_FLAG_ENUM_INDEXED(sgl::check, feature, u8);

namespace sgl::check
{
using feature_set = cc::flags<feature>;

inline constexpr cc::string_view k_feature_names[] = {
    "binding_arrays",              //
    "extended_image_formats",      //
    "readwrite_image_formats",     //
    "multisampled_array_textures", //
    "raytracing",                  //
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
