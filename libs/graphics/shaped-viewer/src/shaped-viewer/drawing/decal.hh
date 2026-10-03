#pragma once

#include <sgl_modules/tracer.hh> // sv::shaders::tracer::decal_record
#include <shaped-rendering/fwd.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

/// A decal as a layer keeps it: the drawing resolved to its records in the drawing manager's decal atlas, and its
/// projector, with the scale folded into the axes.
struct sv::decal_placement
{
    u32 first_record = 0;
    u32 record_count = 0;

    /// The drawing's extent in its own units, which bounds what the projector can paint.
    tg::aabb2f bounds = tg::aabb2f(tg::pos2f(0, 0), tg::pos2f(0, 0));

    tg::pos3f at = tg::pos3f(0, 0, 0);
    tg::vec3f x_axis = tg::vec3f(1, 0, 0);
    tg::vec3f y_axis = tg::vec3f(0, 1, 0);
    f32 depth = 1.0f;

    /// rgba8, sRGB-encoded, straight, red in the low byte.
    u32 tint = 0xffffffff;
};

namespace sv::impl
{
/// `d`'s projector as the tracer reads it, module `tracer`'s `decal_record`, its shapes starting at `first_shape` of the
/// trace's shape buffer.
/// The rows take a world point (x, y, z, 1) to the drawing's own units and to the projection's depth, which is -1 to 1
/// where the projection reaches.
/// The axes must span a plane and `d.depth` must be > 0.
[[nodiscard]] shaders::tracer::decal_record decal_record_of(decal_placement const& d, u32 first_shape);

/// Record `r` as the tracer reads it, module `tracer`'s `decal_shape`.
[[nodiscard]] shaders::tracer::decal_shape decal_shape_of(sr::slug_instance const& r);
} // namespace sv::impl
