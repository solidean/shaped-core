#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-rendering/slug_path.hh>
#include <shaped-rendering/slug_shape.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

// 2D vector content, built once and instanced like a mesh.
// libs/graphics/shaped-viewer/docs/canvas.md is the design.

namespace sv
{
/// Contours of line, quadratic, cubic and arc segments in a drawing's own units, with the shapes as factories.
/// A fill closes every contour; a stroke caps the open ones.
using path = sr::slug_path;
} // namespace sv

/// How one layer of a drawing is filled.
struct sv::fill_style
{
    /// Straight alpha, sRGB-encoded, in [0, 1]: the color a picker shows.
    tg::vec4f color = tg::vec4f(1, 1, 1, 1);

    sr::slug_fill_rule rule = sr::slug_fill_rule::nonzero;
};

/// How one layer of a drawing strokes a path, in the drawing's units: on a canvas and at an anchor, pixels.
struct sv::stroke_style
{
    /// Straight alpha, sRGB-encoded, in [0, 1].
    tg::vec4f color = tg::vec4f(1, 1, 1, 1);

    f32 width = 1.0f;
    sr::stroke_join join = sr::stroke_join::miter;
    sr::stroke_cap cap = sr::stroke_cap::butt;

    /// The longest a miter may reach, in half-widths, before its corner becomes a bevel.
    f32 miter_limit = 4.0f;

    /// Alternating on and off lengths, starting with on; each contour restarts the pattern.
    cc::vector<f32> dashes;
    f32 dash_offset = 0.0f;
};

/// Where one drawing lands inside another: a drawing point (x, y) lands at `at + scale * (x * x_axis + y * y_axis)`.
struct sv::frame_2d
{
    tg::pos2f at = tg::pos2f(0, 0);
    tg::vec2f x_axis = tg::vec2f(1, 0);
    tg::vec2f y_axis = tg::vec2f(0, 1);
    f32 scale = 1.0f;

    /// Multiplies the nested drawing's colors.
    tg::vec4f tint = tg::vec4f(1, 1, 1, 1);
};

/// One 2D graphic: a letter, a logo, an arrow.
///
/// Ordered filled layers, each a closed outline with a color, in the drawing's own units with y pointing down.
/// Later layers draw over earlier ones.
/// A stroke is a layer too: its outline is the area the stroke covers, expanded once when it is added.
/// Nothing here holds a device; a drawing reaches the GPU as part of a `drawing_set`, or alone when instanced directly.
class sv::drawing
{
public:
    struct layer
    {
        sr::slug_outline outline;
        tg::vec4f color = tg::vec4f(1, 1, 1, 1);
    };

    /// Appends a filled layer, every contour of `p` closed by a line back to its start.
    /// The default color is white, which an instance's tint then colors.
    drawing& add_fill(sv::path const& p, fill_style const& style = {});

    /// Appends a filled layer from an outline already built, such as a glyph's; it must be closed.
    drawing& add_fill(sr::slug_outline outline, fill_style const& style = {});

    /// Appends the area `style` covers along `p` as a layer.
    /// Curves stay within 1/4096 of the stroke's extent, so it stays exact up to 4096 px across.
    drawing& add_stroke(sv::path const& p, stroke_style const& style = {});

    /// Appends every layer of `d`, placed by `frame`: a symbol built once and stamped where it is needed.
    /// A copy, not a reference: changing `d` later leaves this drawing as it is.
    drawing& add_drawing(drawing const& d, frame_2d const& frame = {});

    [[nodiscard]] cc::span<layer const> layers() const { return _layers; }
    [[nodiscard]] bool is_empty() const { return _layers.empty(); }

    /// The content key: every layer's curves, contours, fill rule and color, in order.
    /// Computed on the first call after a change, then cached.
    [[nodiscard]] cc::hash128 hash() const;

private:
    cc::vector<layer> _layers;
    mutable cc::hash128 _hash;
    mutable bool _hash_dirty = false;
};

namespace sv::impl
{
/// What acquiring a set last produced: the manager it was acquired by, compared and never followed, and the id it got.
/// A cache, never an identity — a set acquired by a manager that has not seen it gets the id its content hash earns.
struct drawing_set_gpu_slot
{
    drawing_manager const* manager = nullptr;
    drawing_set_id id = drawing_set_id::invalid;
};
} // namespace sv::impl

/// The value a caller builds and keeps: an ordered list of drawings, the 2D counterpart of `sv::mesh`.
///
/// Acquired whole — compiled, placed in the atlas and uploaded once — and hashed as a whole, so a set changed after it
/// was placed is a new set and a new upload.
/// An instance names one drawing of it by the `drawing_id` `add` returned.
class sv::drawing_set
{
public:
    /// Appends `d`, returning its index in this set.
    drawing_id add(drawing d);

    [[nodiscard]] cc::span<drawing const> drawings() const { return _drawings; }
    [[nodiscard]] isize size() const { return _drawings.size(); }
    [[nodiscard]] drawing const& operator[](drawing_id id) const;

    /// The content key, over every drawing's own key in order; computed on first call after a change.
    [[nodiscard]] cc::hash128 hash() const;

    /// What placing this set produced — see `impl::drawing_set_gpu_slot`.
    /// Mutable because placing a set reads it: `canvas_ref::add_drawing` takes a `drawing_set const&`.
    mutable impl::drawing_set_gpu_slot cache;

private:
    cc::vector<drawing> _drawings;
    mutable cc::hash128 _hash;
    mutable bool _hash_dirty = false;
};
