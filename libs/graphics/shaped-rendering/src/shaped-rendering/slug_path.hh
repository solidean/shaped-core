#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/slug_shape.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>

// Paths whose contours may stay open, and strokes: the one kind of content Slug cannot cover directly, expanded on the
// CPU into an outline it can.
// libs/graphics/shaped-rendering/docs/slug.md is the design.

/// One contour of a path: one past its last curve, and whether `close()` ended it.
struct sr::slug_contour
{
    i32 end = 0;
    bool closed = false;
};

/// Contours of quadratic curves that may stay open, in the path's own units: what a stroke follows, and what a fill closes.
///
/// Within a contour each curve starts where the one before it ended.
/// A closed contour's last curve ends where its first started; an open one's need not.
struct sr::slug_path
{
    /// The curves in order, and the contours over them; the builder below is their only writer, so the cursor always
    /// stands where the last curve ended.
    [[nodiscard]] cc::span<slug_curve const> curves() const { return _curves; }
    [[nodiscard]] cc::span<slug_contour const> contours() const { return _contours; }

    /// Moves to `p` without drawing, leaving any contour still open as it is; the next curve starts a contour there.
    slug_path& move_to(tg::pos2f p);

    /// A segment to `p`; one to where the path already is adds nothing.
    /// Without a `move_to` first, a contour starts where the path is: (0, 0), or the start of the contour just closed.
    slug_path& line_to(tg::pos2f p);
    slug_path& quad_to(tg::pos2f control, tg::pos2f p);

    /// A cubic, approximated by as many quadratics as keep the error below `tolerance`, in the path's units.
    slug_path& cubic_to(tg::pos2f c0, tg::pos2f c1, tg::pos2f p, f32 tolerance);

    /// A circular arc around `center`, from where the path is, turning by `sweep`: positive turns from +x toward +y.
    /// `sweep` must be at most 64 full turns either way; a full turn and more draws the circle again.
    /// Quadratics stay within `tolerance` of the circle while the arc needs at most 4096 of them, and stray further past that.
    slug_path& arc_to(tg::pos2f center, tg::angle_f sweep, f32 tolerance);

    /// Ends the open contour with a line back to its start, if it does not end there already, and marks it closed.
    /// With no curve drawn since the last move or close, it does nothing.
    slug_path& close();

    [[nodiscard]] bool is_empty() const { return _curves.empty(); }

    /// Every curve's points, control points included, so the box holds the path but may be larger than it.
    /// An empty path's box is empty at (0, 0).
    [[nodiscard]] tg::aabb2f bounds() const;

    /// The path as a fill sees it: every contour closed by a straight line back to its start.
    [[nodiscard]] slug_outline to_outline(slug_fill_rule rule = slug_fill_rule::nonzero) const;

    // Shapes, each one contour and closed but the polyline.
    // Curved ones stay within 1/4096 of their size of the true curve.

    [[nodiscard]] static slug_path rectangle(tg::aabb2f box);

    /// `radius` is clamped to half the shorter side.
    [[nodiscard]] static slug_path rounded_rectangle(tg::aabb2f box, f32 radius);
    [[nodiscard]] static slug_path circle(tg::pos2f center, f32 radius);
    [[nodiscard]] static slug_path ellipse(tg::pos2f center, tg::vec2f radii);
    [[nodiscard]] static slug_path polygon(cc::span<tg::pos2f const> points);

    /// Open: a stroke ends it with caps, a fill closes it.
    [[nodiscard]] static slug_path polyline(cc::span<tg::pos2f const> points);

private:
    /// Appends `c`, starting a contour at the cursor when none is open.
    void push(slug_curve const& c);

    cc::vector<slug_curve> _curves;
    cc::vector<slug_contour> _contours;

    tg::pos2f _start = tg::pos2f(0, 0);
    tg::pos2f _cursor = tg::pos2f(0, 0);

    /// Whether the last contour in `_contours` still takes curves.
    bool _open = false;
};

/// How a stroke turns a corner between two segments.
enum class sr::stroke_join : sr::u8
{
    /// The outer edges extended until they meet, or a bevel where that point lies past the miter limit.
    miter,
    round,
    bevel,
};

/// How a stroke ends an open contour.
enum class sr::stroke_cap : sr::u8
{
    /// Flat at the end point.
    butt,
    round,

    /// Flat, half the width past the end point.
    square,
};

/// A stroke, in the units of the path it follows.
struct sr::stroke_style
{
    f32 width = 1.0f;
    stroke_join join = stroke_join::miter;
    stroke_cap cap = stroke_cap::butt;

    /// The longest a miter may reach, in half-widths, before its corner becomes a bevel.
    f32 miter_limit = 4.0f;

    /// Alternating on and off lengths, starting with on; an odd count repeats once to make it even.
    /// Empty, all zero or any negative draws a solid line.
    /// Each contour restarts the pattern, and every dash is capped like an open contour.
    /// On a closed contour, a dash running through its start is one dash, joined there.
    /// A zero on length draws that dash's caps alone: a dot under round caps, a square under square, nothing under butt.
    cc::vector<f32> dashes;

    /// How far into the pattern each contour starts.
    f32 dash_offset = 0.0f;
};

namespace sr
{
/// The area `style` covers along `path`, as an outline to fill with the nonzero rule.
///
/// Offset curves are quadratics within `tolerance` of the true offset, in the path's units.
/// The outline is the union of many small contours that overlap, all wound the same way, which is why the rule must be
/// nonzero: an even-odd fill would cut holes where they overlap.
/// A width of zero or less, or a path of no length, gives an empty outline.
[[nodiscard]] slug_outline stroke_outline(slug_path const& path, stroke_style const& style, f32 tolerance);

/// The dashes `style` cuts `path` into, as open contours; a solid style returns `path` itself.
[[nodiscard]] slug_path dash_path(slug_path const& path, stroke_style const& style);
} // namespace sr
