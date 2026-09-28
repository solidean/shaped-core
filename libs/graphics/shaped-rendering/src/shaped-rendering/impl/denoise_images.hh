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
