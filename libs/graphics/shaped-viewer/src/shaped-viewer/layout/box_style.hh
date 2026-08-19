#pragma once

#include <clean-core/container/span.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/vec.hh>

/// Per-side pixel insets.
///
/// The one-argument constructor is implicit, so a uniform inset is written as the number: `{.padding = 8}`.
/// The two- and four-argument forms read CSS-style — `{8, 4}` is horizontal then vertical, `{1, 2, 3, 4}` is left, top,
/// right, bottom.
/// Having constructors at all is what costs this type its designated initializers; the four-argument form is what
/// replaces them.
struct sv::box_insets
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    constexpr box_insets() = default;
    constexpr box_insets(int all) : left(all), top(all), right(all), bottom(all) {}
    constexpr box_insets(int horizontal, int vertical)
      : left(horizontal), top(vertical), right(horizontal), bottom(vertical)
    {
    }
    constexpr box_insets(int left, int top, int right, int bottom) : left(left), top(top), right(right), bottom(bottom)
    {
    }

    [[nodiscard]] static constexpr box_insets all(int v) { return box_insets(v); }
    [[nodiscard]] static constexpr box_insets symmetric(int horizontal, int vertical)
    {
        return box_insets(horizontal, vertical);
    }

    [[nodiscard]] friend constexpr bool operator==(box_insets, box_insets) = default;
};

/// The box properties every layout node carries.
///
/// Outside-in: `margin`, then the `border` band, then `padding`, then the content box the node's children tile.
/// So a node's own rect is its margin box, and only the content box is ever handed to a child.
///
/// `spacing` separates siblings and is read only by the kinds that place more than one child.
/// A border is drawn only when `border_color` has alpha — the width still reserves space either way, which is how an
/// invisible gutter is expressed without a second field.
///
/// `background_color` fills the whole border box, under the border and through the padding, so it is what gives padding
/// and the gutters between children a color — padding has none of its own, exactly as in CSS.
/// It draws before the border and before every child, so both cover it.
struct sv::box_style
{
    box_insets margin = {};
    int border = 0;
    tg::vec4f border_color = tg::vec4f(0, 0, 0, 0);
    tg::vec4f background_color = tg::vec4f(0, 0, 0, 0);
    box_insets padding = {};
    int spacing = 0;
};

namespace sv
{
/// `rect` shrunk by `i` on each side.
/// A rect too small for its insets collapses to empty rather than inverting, so every downstream extent stays >= 0.
[[nodiscard]] tg::aabb2i inset(tg::aabb2i rect, box_insets i);

/// `rect` shrunk by `v` on all four sides.
[[nodiscard]] tg::aabb2i inset(tg::aabb2i rect, int v);

/// The border box of a node occupying `rect` — its margin box less the margin.
[[nodiscard]] tg::aabb2i border_box(tg::aabb2i rect, box_style const& s);

/// The content box of a node occupying `rect` — its border box less the border width and the padding.
/// This is the only rect a child ever sees.
[[nodiscard]] tg::aabb2i content_box(tg::aabb2i rect, box_style const& s);

/// Writes the up-to-four disjoint bands making up `rect`'s border ring of `width` pixels into `out`, and returns how
/// many it wrote.
/// `out` must hold at least four.
///
/// The bands are disjoint and their union is exactly the ring, so drawing them with a blend cannot double-blend a corner.
/// A rect too small for the ring is entirely border, which yields one band rather than four overlapping ones.
[[nodiscard]] int border_bands(tg::aabb2i rect, int width, cc::span<tg::aabb2i> out);

/// Whether a border of this style is worth emitting at all — it must have both a width and a visible color.
[[nodiscard]] inline bool has_visible_border(box_style const& s)
{
    return s.border > 0 && s.border_color[3] > 0.0f;
}

/// Whether a background of this style is worth emitting at all.
/// A width-less concept, unlike a border: the fill is the whole border box, so alpha alone decides.
[[nodiscard]] inline bool has_visible_background(box_style const& s)
{
    return s.background_color[3] > 0.0f;
}
} // namespace sv
