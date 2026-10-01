#pragma once

#include <babel-serializer/font/font.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

// Slug's shapes on the CPU: an outline of quadratic curves, and the same outline compiled into the curve and band data
// the GPU covers it from.
// Nothing here holds a device; sr::slug_atlas is where compiled shapes meet one.
// libs/graphics/shaped-rendering/docs/slug.md is the design.

/// One quadratic Bézier piece of an outline, from `p1` through the control point `p2` to `p3`.
/// A straight line from a to b is {a, b, b}: the control point duplicates the end, as Slug's reference recommends.
struct sr::slug_curve
{
    tg::pos2f p1;
    tg::pos2f p2;
    tg::pos2f p3;
};

/// Which points an outline's contours enclose.
enum class sr::slug_fill_rule : sr::u8
{
    /// Inside wherever the contours wind around a point at all — what every font uses.
    nonzero,

    /// Inside wherever a ray from the point crosses an odd number of contours — what many vector icons use.
    even_odd,
};

/// A shape as closed contours of quadratic curves, in its own units: font units for a glyph, anything for an icon.
///
/// Built with the path calls, or filled directly.
/// Within a contour each curve starts where the one before it ended, and the last ends where the first started.
struct sr::slug_outline
{
    cc::vector<slug_curve> curves;

    /// One past each contour's last curve, ascending.
    cc::vector<i32> contour_ends;

    slug_fill_rule fill_rule = slug_fill_rule::nonzero;

    /// Starts a contour at `p`, closing any contour still open.
    void move_to(tg::pos2f p);
    void line_to(tg::pos2f p);
    void quad_to(tg::pos2f control, tg::pos2f p);

    /// A cubic, approximated by as many quadratics as keep the error below `tolerance`, in the outline's units.
    void cubic_to(tg::pos2f c0, tg::pos2f c1, tg::pos2f p, float tolerance);

    /// Closes the open contour with a line back to its start, if it does not end there already.
    void close();

    [[nodiscard]] bool is_empty() const { return curves.empty(); }

    /// A rectangle as one contour, counter-clockwise.
    [[nodiscard]] static slug_outline rectangle(tg::aabb2f box);

private:
    tg::pos2f _start;
    tg::pos2f _cursor;
    bool _open = false;
};

/// An outline compiled into Slug's two tables, ready for an atlas to place.
///
/// Coordinates are stored as half floats in a *stored space*: the outline's units scaled by `em_scale`, a power of two
/// chosen so the largest coordinate lands at or below 2048, where every integer is exact.
/// The bands are built from the rounded points, so they sort exactly what the GPU reads.
struct sr::slug_compiled_shape
{
    /// Every curve run back to back, one texel per curve plus one closing texel per run.
    /// A texel is four half-float bit patterns: a curve's p1 and p2, or a run's last end point and zeros.
    cc::vector<cc::fixed_array<u16, 4>> curve_texels;

    /// One past each run's last texel, ascending; a run must stay on one row of the curve texture.
    cc::vector<i32> run_ends;

    /// Per horizontal band (bottom to top), the texel of each curve crossing it, by descending maximum x.
    cc::vector<cc::vector<i32>> horizontal_bands;

    /// Per vertical band (left to right), the texel of each curve crossing it, by descending maximum y.
    cc::vector<cc::vector<i32>> vertical_bands;

    /// The shape's box in stored space.
    tg::aabb2f em_bounds;

    /// Stored units per outline unit, a power of two.
    f32 em_scale = 1.0f;

    /// Band scale in xy and offset in zw: a stored-space position times the scale plus the offset is its band index.
    tg::vec4f banding;

    slug_fill_rule fill_rule = slug_fill_rule::nonzero;

    [[nodiscard]] bool is_empty() const { return curve_texels.empty(); }

    /// Texels this shape takes in the band texture: one header entry per band, then every list.
    [[nodiscard]] isize band_texel_count() const;
};

namespace sr
{
/// The outline of a TrueType glyph in font units: implied on-curve points made explicit, composites resolved.
/// Fails on a face whose outlines are not `glyf`, a composite deeper than 16 levels, or a component point index out of range.
[[nodiscard]] cc::result<slug_outline> slug_outline_of(babel::font::face const& face, babel::font::glyph_id glyph);

/// Compiles `outline` into Slug's tables.
/// An empty outline compiles to an empty shape, which draws nothing.
[[nodiscard]] slug_compiled_shape compile_slug_shape(slug_outline const& outline);
} // namespace sr
