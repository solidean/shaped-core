#pragma once

#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/resource/texture_descriptions.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sr::impl
{
[[nodiscard]] inline bool is_set(sg::texture_2d const& t)
{
    return t.raw() != nullptr;
}

[[nodiscard]] inline tg::vec2i extent_of(sg::texture_2d const& t)
{
    return tg::vec2i(t.width(), t.height());
}

/// The largest finite magnitude a texel of `f` holds: a half float's for the 16-bit float formats, which the 11- and
/// 10-bit ones share near enough, and a float's otherwise, where a normalized format saturates on its own.
/// A shader writing a brighter value into a half-float image stores infinity, which a reader after it turns into NaN.
[[nodiscard]] inline float ceiling_of(sg::pixel_format f)
{
    switch (f)
    {
    case sg::pixel_format::r16_float:
    case sg::pixel_format::rg16_float:
    case sg::pixel_format::rgba16_float:
        return 65504.0f;
    case sg::pixel_format::rg11b10_float:
        return 64512.0f;
    default:
        return 3.4028234e38f;
    }
}

/// Makes `image` a readable and writable `format` image of `extent`, creating it only when it is not one already.
/// Returns whether it was created, since a fresh image holds nothing a member may read back as history.
inline bool ensure_image(sg::context& ctx, sg::texture_2d& image, tg::vec2i extent, sg::pixel_format format)
{
    if (is_set(image) && extent_of(image) == extent && image.raw()->format() == format)
        return false;
    image = ctx.persistent.create_texture_2d({.format = format,
                                              .width = extent[0],
                                              .height = extent[1],
                                              .usage = sg::texture_usage::texture | sg::texture_usage::image});
    return true;
}
} // namespace sr::impl
