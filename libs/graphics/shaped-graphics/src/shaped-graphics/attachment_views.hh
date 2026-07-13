#pragma once

#include <clean-core/common/utility.hh>           // cc::move
#include <shaped-graphics/backend/subresource.hh> // subresource_range
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/raw_texture.hh>
#include <shaped-graphics/views.hh>     // texture_view_dimension (shared, raw_view-neutral)
#include <typed-geometry/linalg/vec.hh> // tg::vec4f (clear color)

/// Attachment views — a texture bound as a color (render target) or depth/stencil target of a graphics
/// pipeline. Unlike the shader-binding views in views.hh, these are *not* shader-visible, never enter a
/// binding group / descriptor table, and are bound via the output-merger stage (OMSetRenderTargets /
/// dynamic-rendering attachments). They therefore do not erase to `raw_view`: a backend consumes the
/// typed view directly. Built via `texture<Traits>::as_render_target_view()` / `as_depth_stencil_view()`.

namespace sg
{
/// A render-target (color attachment) view over a single mip level and array-slice range. The texture's
/// format must be a color (renderable) format. Keeps the viewed texture alive via the held handle.
class render_target_view
{
public:
    render_target_view() = default;
    render_target_view(raw_texture_handle texture,
                       texture_view_dimension dimension,
                       pixel_format format,
                       subresource_range range)
      : _texture(cc::move(texture)), _dimension(dimension), _format(format), _range(range)
    {
    }

    [[nodiscard]] raw_texture_handle const& texture() const { return _texture; }
    [[nodiscard]] texture_view_dimension dimension() const { return _dimension; }
    [[nodiscard]] pixel_format format() const { return _format; }
    [[nodiscard]] subresource_range const& range() const { return _range; }

    /// Pixel size of the viewed mip level (mip-adjusted, clamped to at least 1).
    [[nodiscard]] int width() const
    {
        int const w = _texture->width() >> _range.mip_range.start;
        return w < 1 ? 1 : w;
    }
    [[nodiscard]] int height() const
    {
        int const h = _texture->height() >> _range.mip_range.start;
        return h < 1 ? 1 : h;
    }

    // Bind as a rendering-scope target, choosing what happens to its contents at pass start. `&&`
    // overloads move the view into the target; the `const&` overloads copy. See command_list.raster.hh.
    [[nodiscard]] color_target cleared(tg::vec4f color) const&;
    [[nodiscard]] color_target cleared(tg::vec4f color) &&;
    [[nodiscard]] color_target preserved() const&;
    [[nodiscard]] color_target preserved() &&;
    [[nodiscard]] color_target discarded() const&;
    [[nodiscard]] color_target discarded() &&;

private:
    raw_texture_handle _texture;
    texture_view_dimension _dimension = texture_view_dimension::tex_2d;
    pixel_format _format = pixel_format::undefined;
    subresource_range _range;
};

/// A depth-stencil (depth/stencil attachment) view over a single mip level and array-slice range. The
/// texture's format must be a depth (or depth-stencil) format. Keeps the viewed texture alive.
class depth_stencil_view
{
public:
    depth_stencil_view() = default;
    depth_stencil_view(raw_texture_handle texture,
                       texture_view_dimension dimension,
                       pixel_format format,
                       subresource_range range)
      : _texture(cc::move(texture)), _dimension(dimension), _format(format), _range(range)
    {
    }

    [[nodiscard]] raw_texture_handle const& texture() const { return _texture; }
    [[nodiscard]] texture_view_dimension dimension() const { return _dimension; }
    [[nodiscard]] pixel_format format() const { return _format; }
    [[nodiscard]] subresource_range const& range() const { return _range; }

    /// Pixel size of the viewed mip level (mip-adjusted, clamped to at least 1).
    [[nodiscard]] int width() const
    {
        int const w = _texture->width() >> _range.mip_range.start;
        return w < 1 ? 1 : w;
    }
    [[nodiscard]] int height() const
    {
        int const h = _texture->height() >> _range.mip_range.start;
        return h < 1 ? 1 : h;
    }

    // Bind as a rendering-scope target, choosing what happens to its contents at pass start. `&&`
    // overloads move the view into the target; the `const&` overloads copy. See command_list.raster.hh.
    [[nodiscard]] depth_stencil_target cleared(float depth, cc::u8 stencil = 0) const&;
    [[nodiscard]] depth_stencil_target cleared(float depth, cc::u8 stencil = 0) &&;
    [[nodiscard]] depth_stencil_target preserved() const&;
    [[nodiscard]] depth_stencil_target preserved() &&;
    [[nodiscard]] depth_stencil_target discarded() const&;
    [[nodiscard]] depth_stencil_target discarded() &&;

private:
    raw_texture_handle _texture;
    texture_view_dimension _dimension = texture_view_dimension::tex_2d;
    pixel_format _format = pixel_format::undefined;
    subresource_range _range;
};
} // namespace sg
