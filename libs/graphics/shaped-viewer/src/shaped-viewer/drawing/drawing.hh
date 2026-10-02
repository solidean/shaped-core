#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-rendering/slug_shape.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

// 2D vector content, built once and instanced like a mesh.
// libs/graphics/shaped-viewer/docs/canvas.md is the design.

namespace sv
{
/// A closed outline of line, quadratic and cubic segments, in a drawing's own units.
/// Contours must be closed: `close()` after each one.
using path = sr::slug_outline;
} // namespace sv

/// How one layer of a drawing is filled.
struct sv::fill_style
{
    /// Straight alpha, sRGB-encoded, in [0, 1]: the colour a picker shows.
    tg::vec4f color = tg::vec4f(1, 1, 1, 1);

    sr::slug_fill_rule rule = sr::slug_fill_rule::nonzero;
};

/// One 2D graphic: a letter, a logo, an arrow.
///
/// Ordered filled layers, each a closed outline with a colour, in the drawing's own units with y pointing down.
/// Later layers draw over earlier ones.
/// Nothing here holds a device; a drawing reaches the GPU as part of a `drawing_set`, or alone when instanced directly.
class sv::drawing
{
public:
    struct layer
    {
        sv::path outline;
        tg::vec4f color = tg::vec4f(1, 1, 1, 1);
    };

    /// Appends a filled layer; `outline` must be closed.
    /// The default colour is white, which an instance's tint then colours.
    drawing& add_fill(sv::path outline, fill_style const& style = {});

    [[nodiscard]] cc::span<layer const> layers() const { return _layers; }
    [[nodiscard]] bool is_empty() const { return _layers.empty(); }

    /// The content key: every layer's curves, contours, fill rule and colour, in order.
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
