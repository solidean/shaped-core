#include <clean-core/common/utility.hh> // cc::max
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/all.hh>
#include <typed-geometry/linalg/vec_ops.hh> // tg::normalize

using namespace cc::primitive_defines;

// Everything a quadric primitive can be, in one picture.
//
// A quadric is the zero set of a degree-2 polynomial, and a PRIMITIVE is two of them: a surface and a clipper, whose
// interiors intersect.
// That pair is the whole representation, and this is what it reaches — spheres and tubes, but also cones, ellipsoids and
// hyperboloids, none of which is a special case in the record or in the shader.
//
// Left to right:
//
//     sphere           the surface quadric alone, with nothing clipping it
//     ellipsoid        a general quadric: three different semi-axes, which no typed sphere could hold
//     open tube        a cylinder clipped to a slab, the clipper's own surface NOT drawn
//     capped tube      the same record with one bit set, so the slab's two planes are drawn as flat ends
//     capsule          three primitives, because a capsule's surface is not degree 2 and cannot be one
//     hemisphere       a sphere clipped by a plane pair, with the floor drawn
//     cone frustum     a cone clipped by a slab — the clipper cuts BOTH ends, and both are drawn
//     hyperboloid      x^2 + y^2 - z^2 = r^2, the shape that makes the general form worth having
//
// The smaller row behind them is the SAME batch under a non-uniform scale.
// One acceleration structure, one upload, two instances — and the spheres come out as ellipsoids, because a general quadric
// is closed under an affine map where a typed sphere would not be.
//
// Colour is `per_triangle`: one value per primitive, indexed by PrimitiveIndex().
// That is the same frequency a mesh reads a per-face colour at, and it generates the same line of shader code — a quadric
// batch and a triangle mesh differ in the preamble that builds the shading context and in nothing else.
//
// The camera is the FLY one rather than the orbit default, because this is a scene laid out on a floor rather than a
// subject held at arm's length — which is exactly the distinction `sv::camera_style` draws.
// One call picks it; the viewer routes the events and runs the per-frame integration a held key needs.
//
// Controls
//   right-drag   look           W/A/S/D   move along the view         E / Q   rise and fall
//   shift        faster         ctrl      slower                      wheel   retune the base speed
//
// Run it:
//   uv run dev.py example shaped-viewer/quadric-gallery
//   uv run dev.py example shaped-viewer/quadric-gallery --capture

namespace
{
constexpr float spacing = 2.4f;

/// Where the i-th showcase item sits: one row, left to right, centred on the origin.
/// A row rather than a grid because a gallery is read across, and eight of these fit a 16:9 frame without crowding.
tg::pos3f slot(int i)
{
    return tg::pos3f((float(i) - 3.5f) * spacing, 0.0f, 0.0f);
}

/// An ellipsoid with the given semi-axes: x²/a² + y²/b² + z²/c² - 1.
///
/// Built by hand rather than from a factory, which is the point of showing it — the record takes any quadric, and the named
/// constructors are conveniences over the same ten numbers.
sv::quadric_primitive ellipsoid(tg::pos3f const& at, tg::vec3f const& semi_axes)
{
    auto const inv_sq = tg::vec3f(1.0f / (semi_axes[0] * semi_axes[0]), 1.0f / (semi_axes[1] * semi_axes[1]),
                                  1.0f / (semi_axes[2] * semi_axes[2]));

    return {.origin = at,
            .surface = {.diag = inv_sq, .constant = -1.0f},
            .bounds = tg::aabb3f(at - semi_axes, at + semi_axes)};
}

/// A cone about the y axis through `apex`, clipped to `y` in [lo, hi] RELATIVE to it.
///
/// The surface is x² + z² - k² y² = 0, where k is the radius gained per unit of height — a double cone, of which the slab
/// keeps one slice.
/// Both of the slab's planes lie inside the cone, so the frustum's two flat ends are the CLIPPER's own surface: one bit, and
/// no geometry of their own.
sv::quadric_primitive cone_frustum(tg::pos3f const& apex, float slope, float lo, float hi)
{
    auto const mid = (lo + hi) * 0.5f;
    auto const half = (hi - lo) * 0.5f;

    // The widest slice is the one furthest from the apex, on whichever side the slab sits — so it is the larger MAGNITUDE
    // rather than the larger value, which for a slice below the apex would have been negative.
    auto const reach = cc::max(lo < 0.0f ? -lo : lo, hi < 0.0f ? -hi : hi);
    auto const widest = slope * reach;

    return {.origin = apex,
            .surface = {.diag = tg::vec3f(1.0f, -slope * slope, 1.0f)},
            .clip = sv::quadric3::slab(tg::vec3f(0, 1, 0), mid, half),
            .flags = sv::quadric_primitive::flag_emit_clip_surface,
            .bounds = tg::aabb3f(apex + tg::vec3f(-widest, lo, -widest), apex + tg::vec3f(widest, hi, widest))};
}

/// A hyperboloid of one sheet about +y — x² + z² - y² = waist² — clipped to a slab.
///
/// The shape that makes the general form worth having: a saddle surface with no typed counterpart at all, from the same ten
/// numbers and the same intersection routine.
sv::quadric_primitive hyperboloid(tg::pos3f const& at, float waist, float half_height)
{
    auto const widest = tg::sqrt(waist * waist + half_height * half_height);

    return {.origin = at,
            .surface = {.diag = tg::vec3f(1.0f, -1.0f, 1.0f), .constant = -waist * waist},
            .clip = sv::quadric3::slab_about_origin(tg::vec3f(0, 1, 0), half_height),
            .bounds
            = tg::aabb3f(at + tg::vec3f(-widest, -half_height, -widest), at + tg::vec3f(widest, half_height, widest))};
}

/// A sphere cut in half by a plane pair, with the flat face drawn.
sv::quadric_primitive hemisphere(tg::pos3f const& at, float radius)
{
    auto p = sv::quadric_primitive::create_sphere(tg::sphere3f(at, radius));

    // A slab of half-height r/2 centred at r/2 keeps 0 <= y <= r; its lower plane is the floor and its upper one lies
    // outside the sphere, so only the cut shows.
    p.clip = sv::quadric3::slab(tg::vec3f(0, 1, 0), radius * 0.5f, radius * 0.5f);
    p.flags = sv::quadric_primitive::flag_emit_clip_surface;
    return p;
}
} // namespace

EXAMPLE("shaped-viewer/quadric-gallery")
{
    auto gallery = sv::quadric_set();
    gallery.name = "quadric gallery";

    auto colours = cc::vector<tg::vec3f>();

    // One colour per primitive, appended in lockstep with the primitives themselves — which is what keeps a
    // `per_triangle` attribute lined up with `PrimitiveIndex()`.
    auto const push = [&](tg::vec3f const& c) { colours.push_back(c); };

    // 0: a plain sphere — the surface quadric with nothing clipping it.
    gallery.add(tg::sphere3f(slot(0) + tg::vec3f(0, 0.8f, 0), 0.8f));
    push(tg::vec3f(0.85f, 0.30f, 0.25f));

    // 1: an ellipsoid — a general quadric, built from its ten numbers.
    gallery.add(ellipsoid(slot(1) + tg::vec3f(0, 0.8f, 0), tg::vec3f(1.0f, 0.5f, 0.7f)));
    push(tg::vec3f(0.95f, 0.65f, 0.20f));

    // 2: an open tube — the clipper cuts it to length and its own surface is not drawn, so you can see inside.
    gallery.add(tg::segment3f(slot(2) + tg::vec3f(0, 0.25f, 0), slot(2) + tg::vec3f(0, 1.7f, 0)), 0.45f);
    push(tg::vec3f(0.35f, 0.75f, 0.40f));

    // 3: the SAME record with one bit set — the slab's two planes become the flat ends.
    gallery.add(tg::segment3f(slot(3) + tg::vec3f(0, 0.25f, 0), slot(3) + tg::vec3f(0, 1.7f, 0)), 0.45f, true);
    push(tg::vec3f(0.30f, 0.65f, 0.85f));

    // 4: a capsule — three primitives, because its surface is piecewise and cannot be one quadric.
    {
        auto const before = gallery.primitive_count();
        gallery.add_capsule(tg::segment3f(slot(4) + tg::vec3f(0, 0.5f, 0), slot(4) + tg::vec3f(0, 1.5f, 0)), 0.45f);
        for (auto i = before; i < gallery.primitive_count(); ++i)
            push(tg::vec3f(0.70f, 0.40f, 0.85f));
    }

    // 5: a hemisphere, floor drawn.
    gallery.add(hemisphere(slot(5), 1.0f));
    push(tg::vec3f(0.90f, 0.85f, 0.35f));

    // 6: a cone frustum — the clipper cuts both ends and both are drawn.
    // The apex is above it and the slice is taken from the lower nappe, so it sits wide-end down on the floor.
    gallery.add(cone_frustum(slot(6) + tg::vec3f(0, 2.2f, 0), 0.55f, -2.2f, -0.7f));
    push(tg::vec3f(0.95f, 0.45f, 0.55f));

    // 7: a hyperboloid of one sheet, clipped to a slab — no typed primitive holds this shape at all.
    gallery.add(hyperboloid(slot(7) + tg::vec3f(0, 1.0f, 0), 0.45f, 0.9f));
    push(tg::vec3f(0.45f, 0.85f, 0.85f));

    gallery.attributes.push_back(
        sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_triangle, cc::move(colours)));

    // The floor, so the shapes sit on something and cast shadows onto it.
    auto const floor
        = sv::mesh{.name = "floor",
                   .geometry = sv::triangle_geometry::create_from_positions(
                       cc::vector<tg::pos3f>{tg::pos3f(-13, 0, -4), tg::pos3f(13, 0, -4), tg::pos3f(13, 0, 9),
                                             tg::pos3f(-13, 0, -4), tg::pos3f(13, 0, 9), tg::pos3f(-13, 0, 9)}),
                   .attributes = {sv::mesh_attribute::create_value("base_color", tg::vec3f(0.22f, 0.23f, 0.25f))}};

    for (auto f : sv::interactive("shaped-viewer/quadric-gallery"))
    {
        auto view = f.window().view();

        // Where the walk starts, applied the first time this view id is seen; after that wherever it has been flown to wins.
        view.initial_fps({.position = tg::pos3d(-1.5, 3.2, -11.0),
                          .yaw = tg::angle_d::make_from_degree(8.0),
                          .pitch = tg::angle_d::make_from_degree(-11.0)});

        // Re-asserted every frame, like `movable`: the viewer routes input against the PREVIOUS frame's answer, so a call
        // that stops being made hands the view back to the orbit controller.
        view.camera_style(sv::camera_style::fly);

        auto scene = view.add_scene();
        scene.add_mesh(floor);

        // The gallery as authored.
        scene.add_quadrics(gallery);

        // ...and the same batch again, squashed and pushed back: one upload and one acceleration structure, two instances.
        // A non-uniform scale turns every sphere in it into an ellipsoid, which costs nothing because a general quadric is
        // closed under an affine map.
        auto instanced = scene.add_quadrics(gallery);
        instanced.transform(
            tg::compose(tg::affine_transform3f(tg::rigid_transform3f::make_translation(tg::vec3f(0, 0, 4.6f))),
                        tg::affine_transform3f(tg::scaling_transform3f::make_scaling(tg::vec3f(0.5f, 0.32f, 0.5f)))));

        scene.add_light({.center = tg::pos3f(-2.0f, 7.5f, -1.0f),
                         .half_extent_u = tg::vec3f(3.0f, 0, 0),
                         .half_extent_v = tg::vec3f(0, 0, 3.0f),
                         .emission = tg::vec3f(9.0f, 9.0f, 9.0f)});

        scene.background(sv::background::gradient(tg::vec3f(0.38f, 0.44f, 0.58f), tg::vec3f(0.10f, 0.11f, 0.14f)));
    }
}
