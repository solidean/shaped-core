#pragma once

#include <shaped-rendering/fwd.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

/// The corner of a view a 2D instance's position is measured from; offsets always point into the view.
enum class sv::corner : sv::u8
{
    top_left,
    top_right,
    bottom_left,
    bottom_right,
};

/// One placement of a drawing on a canvas, in the view's logical pixels with y down.
///
/// A drawing point (x, y) lands at `at + scale * (x * x_axis + y * y_axis)`, measured from `from`.
/// From a right or bottom corner, `at` is how far the placed drawing's far edge sits in from that edge, so it stays inside.
/// The axes are free: their lengths and angle stretch and shear the drawing.
struct sv::instance_2d
{
    tg::pos2f at = tg::pos2f(0, 0);
    tg::vec2f x_axis = tg::vec2f(1, 0);
    tg::vec2f y_axis = tg::vec2f(0, 1);
    f32 scale = 1.0f;

    /// Multiplies the drawing's own colors; straight alpha, sRGB-encoded, in [0, 1].
    tg::vec4f tint = tg::vec4f(1, 1, 1, 1);

    sv::corner from = sv::corner::top_left;
};

/// One placement of a drawing in a scene, in world units.
///
/// A drawing point (x, y) lands at `at + scale * (x * x_axis + y * y_axis)`.
/// The drawing's y points down, so a label meant to read upright passes the surface's downward direction as `y_axis`.
/// The axes have no default, since a plane in 3D has to be said.
struct sv::instance_3d
{
    tg::pos3f at;
    tg::vec3f x_axis;
    tg::vec3f y_axis;
    f32 scale = 1.0f;

    /// Multiplies the drawing's own colors; straight alpha, sRGB-encoded, in [0, 1].
    tg::vec4f tint = tg::vec4f(1, 1, 1, 1);
};

/// A drawing projected onto the traced surfaces of a scene, which it paints as part of their material.
///
/// The drawing is placed on a plane as `instance_3d` places it, and projected along `y_axis × x_axis` — away from a
/// viewer who reads it the right way round — to `depth` world units on either side of that plane.
/// Every surface inside that box takes the drawing's colors as its base color on the side facing the projector, and is
/// lit, shadowed and reflected with it, whichever way its mesh is wound.
/// A surface met at a grazing angle fades out instead of smearing the drawing along it.
/// The axes should be orthogonal, since the projection runs along their cross product.
/// The axes start at zero, which is no plane, since a plane in 3D has to be said.
struct sv::decal
{
    tg::pos3f at = tg::pos3f(0, 0, 0);
    tg::vec3f x_axis = tg::vec3f(0, 0, 0);
    tg::vec3f y_axis = tg::vec3f(0, 0, 0);
    f32 scale = 1.0f;

    /// How far the projection reaches either side of the plane, in world units; must be > 0.
    f32 depth = 1.0f;

    /// Multiplies the drawing's own colors; straight alpha, sRGB-encoded, in [0, 1].
    tg::vec4f tint = tg::vec4f(1, 1, 1, 1);
};

/// One instance as a layer keeps it: the drawing resolved to its records in the manager's atlas, and the frame placing it.
/// `scale` is already folded into the axes; a 2D placement's `at` is still measured from `from`, which needs the view's size.
struct sv::drawing_placement
{
    drawing_set_id set = drawing_set_id::invalid;

    /// The atlas page `set` lives in, which is the atlas a job draws this placement's records from.
    u32 page = 0;
    u32 first_record = 0;
    u32 record_count = 0;

    tg::pos3f at = tg::pos3f(0, 0, 0);
    tg::vec3f x_axis = tg::vec3f(1, 0, 0);
    tg::vec3f y_axis = tg::vec3f(0, 1, 0);

    /// rgba8, sRGB-encoded, straight, red in the low byte.
    u32 tint = 0xffffffff;

    sv::corner from = sv::corner::top_left;

    /// 2D: how far the placed block reaches right of and below its anchor, so a right or bottom corner aligns its far
    /// edge; and where this placement sits within that block, which a corner leaves as it is.
    /// A drawing is a block of its own, at offset zero; a glyph of a string is one of many in its string's block.
    tg::vec2f reach = tg::vec2f(0, 0);
    tg::vec2f offset = tg::vec2f(0, 0);

    /// Whether the placement draws by a probe of the trace's depth, and where and at what depth it probes.
    /// Value-initialized, it is `sr::slug_visibility::always` and nothing is probed.
    sr::slug_visibility visibility = {};
    tg::vec2f probe = tg::vec2f(0, 0);
    f32 probe_depth = 0.0f;
};
