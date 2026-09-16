#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/scene/quadric.hh>
#include <shaped-viewer/scene/quadric_set.hh>
#include <typed-geometry/linalg/vec_ops.hh> // tg::dot, tg::normalize

using namespace cc::primitive_defines;

// CPU-only tests for the quadric primitive record and the reference intersection the shader is written against.
//
// No device: a quadric_primitive is a value, and everything here is the arithmetic the intersection shader will mirror.
// Two properties are worth naming, because both are easy to lose and neither shows up as a crash.
//
// The far-root fallback is what makes an OPEN clipped cylinder visible end-on; without it an edge pointing at the camera
// disappears, which is the common view rather than an exotic one.
// And the per-primitive origin is what keeps float32 honest — a world-space quadric loses a small radius entirely at mesh scale,
// so `far_from_the_world_origin` below is a correctness test rather than a robustness one.

namespace
{
constexpr float eps = 1e-4f;

tg::ray3f ray_from(tg::pos3f const& origin, tg::vec3f const& dir)
{
    return tg::ray3f(origin, tg::normalize(dir));
}

bool near(float a, float b, float tol = eps)
{
    return a - b < tol && b - a < tol;
}

bool near(tg::vec3f const& a, tg::vec3f const& b, float tol = eps)
{
    return near(a[0], b[0], tol) && near(a[1], b[1], tol) && near(a[2], b[2], tol);
}

bool near(tg::pos3f const& a, tg::pos3f const& b, float tol = eps)
{
    return near(a[0], b[0], tol) && near(a[1], b[1], tol) && near(a[2], b[2], tol);
}
} // namespace

TEST("sv::quadric3 evaluates and differentiates a sphere")
{
    auto const q = sv::quadric3::sphere_about_origin(2.0f);

    CHECK(near(q.evaluate(tg::vec3f(0, 0, 0)), -4.0f)); // the centre is inside
    CHECK(near(q.evaluate(tg::vec3f(2, 0, 0)), 0.0f));  // and the surface is the zero set
    CHECK(near(q.evaluate(tg::vec3f(3, 0, 0)), 5.0f));  // outside is positive

    // The gradient points outward, which is what makes it the normal without a sign fix.
    CHECK(near(tg::normalize(q.gradient(tg::vec3f(2, 0, 0))), tg::vec3f(1, 0, 0)));
    CHECK(near(tg::normalize(q.gradient(tg::vec3f(0, -2, 0))), tg::vec3f(0, -1, 0)));
}

TEST("sv::quadric3 slab and cylinder agree with their closed forms")
{
    auto const axis = tg::vec3f(0, 0, 1);

    auto const cyl = sv::quadric3::cylinder_about_origin(axis, 1.0f);
    CHECK(near(cyl.evaluate(tg::vec3f(1, 0, 0)), 0.0f));   // on the surface
    CHECK(near(cyl.evaluate(tg::vec3f(1, 0, 100)), 0.0f)); // and still on it, however far along the axis
    CHECK(cyl.evaluate(tg::vec3f(0, 0, 50)) < 0.0f);       // the axis is inside

    auto const slab = sv::quadric3::slab_about_origin(axis, 3.0f);
    CHECK(slab.evaluate(tg::vec3f(9, 9, 0)) < 0.0f); // the slab does not care about the other two axes
    CHECK(near(slab.evaluate(tg::vec3f(0, 0, 3)), 0.0f));
    CHECK(slab.evaluate(tg::vec3f(0, 0, 4)) > 0.0f);

    // A quadric with nothing to clip admits every point, which is what an unclipped sphere carries.
    auto const all = sv::quadric3::everywhere();
    CHECK(all.evaluate(tg::vec3f(0, 0, 0)) < 0.0f);
    CHECK(all.evaluate(tg::vec3f(1e6f, -1e6f, 1e6f)) < 0.0f);
}

TEST("sv::intersect reports the near root of a sphere, with an outward normal")
{
    auto const p = sv::quadric_primitive::create_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f));

    auto const hit = sv::intersect(p, ray_from(tg::pos3f(-5, 0, 0), tg::vec3f(1, 0, 0)));
    REQUIRE(hit.has_value());
    CHECK(near(hit.value().t, 4.0f));
    CHECK(near(hit.value().normal, tg::vec3f(-1, 0, 0))); // the face the ray arrived at

    // A ray pointing away has both roots behind it.
    CHECK(!sv::intersect(p, ray_from(tg::pos3f(-5, 0, 0), tg::vec3f(-1, 0, 0))).has_value());

    // And one that passes by hits nothing at all.
    CHECK(!sv::intersect(p, ray_from(tg::pos3f(-5, 3, 0), tg::vec3f(1, 0, 0))).has_value());
}

TEST("sv::intersect falls through to the far root from inside a sphere")
{
    auto const p = sv::quadric_primitive::create_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 2.0f));

    // The near root is behind the origin, so the only hit in range is the far wall.
    auto const hit = sv::intersect(p, ray_from(tg::pos3f(0, 0, 0), tg::vec3f(0, 1, 0)));
    REQUIRE(hit.has_value());
    CHECK(near(hit.value().t, 2.0f));

    // The normal there still points out of the surface rather than back at the ray.
    CHECK(near(hit.value().normal, tg::vec3f(0, 1, 0)));
}

TEST("sv::intersect clips a cylinder to its segment")
{
    auto const p = sv::quadric_primitive::create_cylinder(tg::segment3f(tg::pos3f(0, 0, -1), tg::pos3f(0, 0, 1)), 0.5f);

    // Broadside, within the slab.
    auto const hit = sv::intersect(p, ray_from(tg::pos3f(-4, 0, 0), tg::vec3f(1, 0, 0)));
    REQUIRE(hit.has_value());
    CHECK(near(hit.value().t, 3.5f));
    CHECK(near(hit.value().normal, tg::vec3f(-1, 0, 0)));

    // Broadside, past the end: the infinite cylinder is hit and the clipper rejects both roots.
    CHECK(!sv::intersect(p, ray_from(tg::pos3f(-4, 0, 2), tg::vec3f(1, 0, 0))).has_value());
}

TEST("sv::intersect sees the far wall of an open cylinder end-on")
{
    // The case the far-root fallback exists for, and the one an edge pointing at the camera produces.
    // A slab-clipped cylinder has no caps, so looking down its axis the near root is outside the slab and what is
    // actually visible is the inside of the opposite wall.
    auto const p = sv::quadric_primitive::create_cylinder(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 0, 10)), 1.0f);

    // Slightly off-axis so the ray meets the wall rather than running parallel to it forever.
    auto const hit = sv::intersect(p, ray_from(tg::pos3f(0, 0, -1), tg::vec3f(0.1f, 0, 1)));
    REQUIRE(hit.has_value());

    auto const at = tg::pos3f(0, 0, -1) + tg::normalize(tg::vec3f(0.1f, 0, 1)) * hit.value().t;
    CHECK(at[2] > 0.0f); // inside the slab
    CHECK(at[2] < 10.0f);
    CHECK(near(at[0] * at[0] + at[1] * at[1], 1.0f, 1e-3f)); // and on the wall

    // The surface normal there points out of the cylinder, so it faces AWAY from the viewer — an inside view.
    CHECK(tg::dot(hit.value().normal, tg::normalize(tg::vec3f(0.1f, 0, 1))) > 0.0f);
}

TEST("sv::quadric_primitive bounds a cylinder exactly")
{
    // An axis-aligned cylinder adds nothing along its own axis, which is what makes the box tight rather than merely correct.
    // A sphere per endpoint would have overshot by a full radius at each end.
    auto const p = sv::quadric_primitive::create_cylinder(tg::segment3f(tg::pos3f(0, 0, -2), tg::pos3f(0, 0, 3)), 0.5f);

    CHECK(near(p.bounds.min[2], -2.0f));
    CHECK(near(p.bounds.max[2], 3.0f));
    CHECK(near(p.bounds.min[0], -0.5f));
    CHECK(near(p.bounds.max[0], 0.5f));

    // A sphere's box is the obvious one.
    auto const s = sv::quadric_primitive::create_sphere(tg::sphere3f(tg::pos3f(1, 2, 3), 0.25f));
    CHECK(near(s.bounds.min[0], 0.75f));
    CHECK(near(s.bounds.max[1], 2.25f));
}

TEST("sv::quadric_primitive survives far from the world origin")
{
    // The reason `origin` is in the record at all.
    // Written as a world-space quadric, this sphere's constant term is |c|^2 - r^2 = 2.5e7 - 6.4e-5, and the ulp of 2.5e7 in
    // float32 is about 2 — so the radius would be gone before anything read it, and the primitive would vanish or balloon.
    auto const centre = tg::pos3f(5000.0f, -3000.0f, 1200.0f);
    float const radius = 0.008f;

    auto const p = sv::quadric_primitive::create_sphere(tg::sphere3f(centre, radius));

    auto const hit = sv::intersect(p, ray_from(centre - tg::vec3f(1, 0, 0), tg::vec3f(1, 0, 0)));
    REQUIRE(hit.has_value());
    CHECK(near(hit.value().t, 1.0f - radius, 1e-3f));

    // The silhouette is what a lost radius destroys, so check a ray that must MISS by a hair as well as one that must hit.
    CHECK(
        sv::intersect(p, ray_from(centre - tg::vec3f(1, 0, 0) + tg::vec3f(0, 0.004f, 0), tg::vec3f(1, 0, 0))).has_value());
    CHECK(
        !sv::intersect(p, ray_from(centre - tg::vec3f(1, 0, 0) + tg::vec3f(0, 0.02f, 0), tg::vec3f(1, 0, 0))).has_value());
}

TEST("sv::capsule_primitives returns a cylinder and two caps")
{
    auto const prims = sv::capsule_primitives(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 0, 4)), 1.0f);

    REQUIRE(prims.size() == 3);

    // The cap the flat-capped form leaves off: a point beyond the segment's end is on the capsule and not on the cylinder.
    auto const beyond = tg::pos3f(0, 0, 4.5f);
    CHECK(!prims[0].admits(beyond));
    CHECK(prims[2].admits(beyond));

    // The union's box reaches a full radius past each end, where the cylinder alone stops at the end.
    CHECK(near(prims[0].bounds.max[2], 4.0f));
    CHECK(near(prims[2].bounds.max[2], 5.0f));
}

TEST("sv::line_primitives spells the three ends")
{
    auto const s = tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 0, 4));

    // round is the capsule; the other two are one primitive and differ only in the emit bit.
    CHECK(sv::line_primitives(s, {.radius = 1.0f, .ends = sv::line_ends::round}).size() == 3);

    auto const open = sv::line_primitives(s, {.radius = 1.0f, .ends = sv::line_ends::open});
    auto const flat = sv::line_primitives(s, {.radius = 1.0f, .ends = sv::line_ends::flat});
    REQUIRE(open.size() == 1);
    REQUIRE(flat.size() == 1);

    CHECK(!open[0].emits_clip_surface());
    CHECK(flat[0].emits_clip_surface());

    // The box must not move with the bit — the acceleration structure's box bounds the solid, never the visible part.
    CHECK(open[0].bounds.min == flat[0].bounds.min);
    CHECK(open[0].bounds.max == flat[0].bounds.max);

    // And the surface quadrics are the same record either way.
    CHECK(open[0].surface == flat[0].surface);
    CHECK(open[0].clip == flat[0].clip);
}

TEST("sv::quadric_primitive handles a degenerate segment")
{
    // No axis to build a cylinder about, so the honest answer is the sphere the degenerate segment describes.
    auto const p = sv::quadric_primitive::create_cylinder(tg::segment3f(tg::pos3f(1, 1, 1), tg::pos3f(1, 1, 1)), 0.5f);

    CHECK(p.clip == sv::quadric3::everywhere());
    CHECK(near(p.bounds.max[0], 1.5f));

    auto const hit = sv::intersect(p, ray_from(tg::pos3f(1, 1, -4), tg::vec3f(0, 0, 1)));
    REQUIRE(hit.has_value());
    CHECK(near(hit.value().t, 4.5f));
}

TEST("sv::intersect draws no cap on an open cylinder")
{
    // The default, and what a wireframe wants: the ends are where the clipper cuts, and nothing closes them.
    auto const p = sv::quadric_primitive::create_cylinder(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 0, 4)), 1.0f);
    CHECK(!p.emits_clip_surface());

    // Angled, not parallel: a ray running exactly along the axis inside the tube never meets the wall at all, so it would
    // say nothing about whether the CAP was drawn.
    // This one crosses the cap plane at z = 0 well inside the radius, then reaches the wall at z ~ 1.86.
    auto const origin = tg::pos3f(0, 0, -1);
    auto const dir = tg::vec3f(0.35f, 0, 1);

    auto const hit = sv::intersect(p, ray_from(origin, dir));
    REQUIRE(hit.has_value());

    auto const at = origin + tg::normalize(dir) * hit.value().t;
    CHECK(at[2] > 0.1f);                                     // past the cap plane, so nothing was drawn there
    CHECK(near(at[0] * at[0] + at[1] * at[1], 1.0f, 1e-3f)); // and on the wall instead
}

TEST("sv::intersect draws the cap of a capped cylinder")
{
    auto const p
        = sv::quadric_primitive::create_cylinder(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 0, 4)), 1.0f, true);
    CHECK(p.emits_clip_surface());

    // The SAME ray now stops on the flat end at z = 0, which is the clipper's own surface rather than the cylinder's.
    auto const origin = tg::pos3f(0, 0, -1);
    auto const dir = tg::vec3f(0.35f, 0, 1);

    auto const hit = sv::intersect(p, ray_from(origin, dir));
    REQUIRE(hit.has_value());

    auto const at = origin + tg::normalize(dir) * hit.value().t;
    CHECK(near(at[2], 0.0f));
    CHECK(near(at[0], 0.35f));

    // Its normal is the cap's, along the axis back at the ray rather than radially outward.
    CHECK(near(hit.value().normal, tg::vec3f(0, 0, -1)));

    // A ray crossing the cap plane OUTSIDE the cylinder's radius still misses: the cap is a surface only where it lies
    // inside the other quadric, which is the whole interval test.
    CHECK(!sv::intersect(p, ray_from(tg::pos3f(3, 0, -1), tg::vec3f(0, 0, 1))).has_value());
}

TEST("sv::quadric_primitive bounds the solid, not the visible part of it")
{
    // The invariant the acceleration structure rests on: the box is the same whether or not the caps are drawn.
    // A box that tracked visibility would make one geometry two resources, and a dropped hit a silent hole.
    auto const seg = tg::segment3f(tg::pos3f(-1, 2, 0.5f), tg::pos3f(3, -1, 2));

    auto const open = sv::quadric_primitive::create_cylinder(seg, 0.4f);
    auto const capped = sv::quadric_primitive::create_cylinder(seg, 0.4f, true);

    CHECK(open.bounds.min == capped.bounds.min);
    CHECK(open.bounds.max == capped.bounds.max);

    // And the two differ in exactly one thing.
    CHECK(open.surface == capped.surface);
    CHECK(open.clip == capped.clip);
    CHECK(open.flags != capped.flags);
}

TEST("sv::intersect draws a hemisphere with or without its floor")
{
    // A sphere clipped by a plane pair — the other shape the clipper's own surface is wanted for.
    // The slab's half-height is the sphere's radius, so one of its two planes lies outside and only the cut at z = 0 shows.
    auto const dome = [](bool floor)
    {
        auto p = sv::quadric_primitive::create_sphere(tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f));
        p.clip = sv::quadric3::slab(tg::vec3f(0, 0, 1), 0.5f, 0.5f); // keeps 0 <= z <= 1
        p.flags = floor ? sv::quadric_primitive::flag_emit_clip_surface : 0u;
        return p;
    };

    // Looking up from below at the flat side.
    auto const ray = ray_from(tg::pos3f(0.2f, 0, -3), tg::vec3f(0, 0, 1));

    auto const without = sv::intersect(dome(false), ray);
    REQUIRE(without.has_value());
    auto const at_without = tg::pos3f(0.2f, 0, -3) + tg::vec3f(0, 0, 1) * without.value().t;
    CHECK(at_without[2] > 0.0f); // the floor is not there, so the dome's inner surface is what is hit

    auto const with = sv::intersect(dome(true), ray);
    REQUIRE(with.has_value());
    CHECK(near(with.value().t, 3.0f)); // the floor, at z = 0
    CHECK(near(with.value().normal, tg::vec3f(0, 0, -1)));
}

TEST("sv::intersect handles a cone frustum")
{
    // A cone about +y through the origin: x² + z² - k² y² = 0, sliced by a slab.
    // Nothing constructs one of these outside the gallery example, and the example is where a signed-extent bug in its
    // BOUNDS went unnoticed until the picture looked wrong — so the shape gets a test of its own.
    constexpr float slope = 0.5f;

    auto p = sv::quadric_primitive();
    p.origin = tg::pos3f(0, 0, 0);
    p.surface = {.diag = tg::vec3f(1.0f, -slope * slope, 1.0f)};
    p.clip = sv::quadric3::slab(tg::vec3f(0, 1, 0), 2.0f, 1.0f); // keeps 1 <= y <= 3
    p.flags = sv::quadric_primitive::flag_emit_clip_surface;

    // Broadside at y = 2, where the cone's radius is slope * 2 = 1.
    auto const side = sv::intersect(p, ray_from(tg::pos3f(-5, 2, 0), tg::vec3f(1, 0, 0)));
    REQUIRE(side.has_value());
    CHECK(near(side.value().t, 4.0f));

    // Straight down the axis onto the wide end at y = 3, which is the CLIPPER's surface.
    auto const cap = sv::intersect(p, ray_from(tg::pos3f(0.4f, 6, 0), tg::vec3f(0, -1, 0)));
    REQUIRE(cap.has_value());
    CHECK(near(cap.value().t, 3.0f));
    CHECK(near(cap.value().normal, tg::vec3f(0, 1, 0)));

    // Below the slab there is nothing, even though the infinite double cone continues through it.
    CHECK(!sv::intersect(p, ray_from(tg::pos3f(-5, 0.5f, 0), tg::vec3f(1, 0, 0))).has_value());

    // And outside the widest slice, likewise.
    CHECK(!sv::intersect(p, ray_from(tg::pos3f(-5, 2, 3), tg::vec3f(1, 0, 0))).has_value());
}

TEST("sv::intersect handles a hyperboloid of one sheet")
{
    // x² + z² - y² = waist², the shape with no typed counterpart at all — and the one that most exercises a quadric whose
    // quadratic part is indefinite, where the two roots straddle the waist rather than bracketing a convex body.
    constexpr float waist = 1.0f;

    auto p = sv::quadric_primitive();
    p.surface = {.diag = tg::vec3f(1.0f, -1.0f, 1.0f), .constant = -waist * waist};
    p.clip = sv::quadric3::slab_about_origin(tg::vec3f(0, 1, 0), 2.0f);

    // At the waist, y = 0, the radius is exactly `waist`.
    auto const at_waist = sv::intersect(p, ray_from(tg::pos3f(-5, 0, 0), tg::vec3f(1, 0, 0)));
    REQUIRE(at_waist.has_value());
    CHECK(near(at_waist.value().t, 4.0f));

    // Higher up it flares: at y = 2 the radius is sqrt(1 + 4).
    auto const flared = sv::intersect(p, ray_from(tg::pos3f(-5, 1.999f, 0), tg::vec3f(1, 0, 0)));
    REQUIRE(flared.has_value());
    CHECK(near(flared.value().t, 5.0f - tg::sqrt(1.0f + 1.999f * 1.999f), 1e-2f));

    // A ray through the throat along the axis meets the surface nowhere, because the surface never crosses it.
    CHECK(!sv::intersect(p, ray_from(tg::pos3f(0, -5, 0), tg::vec3f(0, 1, 0))).has_value());

    // Outside the slab there is nothing, though the surface itself continues.
    CHECK(!sv::intersect(p, ray_from(tg::pos3f(-9, 3, 0), tg::vec3f(1, 0, 0))).has_value());
}

TEST("sv::quadric_primitive::create_cone builds the frustum's limiting case")
{
    // The same shape the frustum test builds by hand, but tipped: the factory takes a base circle and an apex, which is the
    // pair every caller actually has.
    auto const p = sv::quadric_primitive::create_cone(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 4, 0)), 2.0f);

    // Broadside at y = 1, where three quarters of the height is left and the radius is therefore 1.5.
    auto const side = sv::intersect(p, ray_from(tg::pos3f(-5, 1, 0), tg::vec3f(1, 0, 0)));
    REQUIRE(side.has_value());
    CHECK(near(side.value().t, 3.5f));

    // The base disc is the clipper's own surface, and it faces away from the apex.
    auto const disc = sv::intersect(p, ray_from(tg::pos3f(0.5f, -3, 0), tg::vec3f(0, 1, 0)));
    REQUIRE(disc.has_value());
    CHECK(near(disc.value().t, 3.0f));
    CHECK(near(disc.value().normal, tg::vec3f(0, -1, 0)));

    // The OTHER nappe is what a cone quadric cannot tell apart from this one, and the slab is what discards it: below the
    // base the double cone continues, and nothing there is hit.
    CHECK(!sv::intersect(p, ray_from(tg::pos3f(-5, -1, 0), tg::vec3f(1, 0, 0))).has_value());

    // Past the apex, likewise.
    CHECK(!sv::intersect(p, ray_from(tg::pos3f(-5, 5, 0), tg::vec3f(1, 0, 0))).has_value());

    // The box is the solid's: the base disc's extent, and the apex.
    CHECK(near(p.bounds.min, tg::pos3f(-2, 0, -2)));
    CHECK(near(p.bounds.max, tg::pos3f(2, 4, 2)));
}

TEST("sv::quadric_primitive::create_cone is axis-agnostic")
{
    // The general-axis form is the whole reason for `cone_about_origin`: every cone before this one was built about +y by
    // hand, and an arrow points wherever it is asked to.
    auto const apex = tg::pos3f(3, -1, 2);
    auto const base = tg::pos3f(1, 1, 1);
    auto const p = sv::quadric_primitive::create_cone(tg::segment3f(base, apex), 0.5f);

    // The apex is ON the surface, and so is the rim: a point at the base, one radius off the axis.
    CHECK(near(p.surface.evaluate(apex - p.origin), 0.0f));

    auto const axis = tg::normalize(apex - base);
    auto const any = tg::vec3f(0, 0, 1);
    auto const off = tg::normalize(any - axis * tg::dot(any, axis));
    auto const rim = base + off * 0.5f;
    CHECK(near(p.surface.evaluate(rim - p.origin), 0.0f, 1e-3f));

    // The clipper admits the interior; both of its planes pass through points tested above, so this asks halfway up the axis
    // rather than on a boundary float32 will not land on exactly.
    CHECK(p.admits(base + (apex - base) * 0.5f));

    // The base's centre is inside the cone and admitted; a point a radius past the rim is neither.
    CHECK(p.surface.evaluate(base - p.origin) < 0.0f);
    CHECK(p.surface.evaluate(base + off * 1.0f - p.origin) > 0.0f);

    // And the mirrored nappe, the same distance the other side of the apex, is on the surface but clipped away.
    auto const mirrored = apex + (apex - rim);
    CHECK(near(p.surface.evaluate(mirrored - p.origin), 0.0f, 1e-3f));
    CHECK(!p.admits(mirrored));
}

TEST("sv::arrow_primitives puts the tip on pos1")
{
    // The property that makes an arrow measure something: it spans exactly the segment it is given, head included.
    auto const tip = tg::pos3f(0, 3, 0);
    auto const prims = sv::arrow_primitives(tg::segment3f(tg::pos3f(0, 0, 0), tip));
    REQUIRE(prims.size() == 2);

    auto const& shaft = prims[0];
    auto const& head = prims[1];

    // `create_cone` origins the primitive at the apex, so this is the tip itself rather than a point near it.
    CHECK(near(head.origin, tip));

    // A length of 3 gives a shaft radius of 0.06 and a head 0.45 long, so the shaft runs from 0 to 2.55.
    auto const style = sv::arrow_style::for_length(3.0f);
    CHECK(near(style.shaft_radius, 0.06f));
    CHECK(near(style.head_length, 0.45f));
    CHECK(near(shaft.origin, tg::pos3f(0, (3.0f - 0.45f) * 0.5f, 0)));

    // The two boxes together span the segment and nothing beyond it.
    CHECK(near(shaft.bounds.min[1], 0.0f));
    CHECK(near(head.bounds.max[1], 3.0f));

    // The head is the wider of the two, which is what hides the shaft's far cap.
    CHECK(style.head_radius > style.shaft_radius);
}

TEST("sv::arrow_primitives clamps a head longer than the arrow")
{
    // A short arrow with a fixed shaft radius is the case this exists for: a vector field's shortest vectors would otherwise
    // get an inside-out shaft, which is a hit at a negative extent rather than a missing one.
    auto const s = tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0.1f, 0, 0));
    auto const prims = sv::arrow_primitives(s, sv::arrow_style::for_shaft_radius(0.05f)); // a head 0.375 long

    REQUIRE(prims.size() == 1);
    CHECK(near(prims[0].origin, s.pos1));
    CHECK(near(prims[0].bounds.min[0], 0.0f)); // the head alone, still spanning the segment

    // A segment with no length has no direction for a head to point, so there is no arrow rather than a degenerate one.
    CHECK(sv::arrow_primitives(tg::segment3f(s.pos0, s.pos0)).empty());
}

TEST("sv::arrow_style scales the head with the shaft")
{
    // What the overloads mean: the no-style form is the proportional one, and the float form fixes the thickness so that
    // length is the only thing an arrow's size encodes.
    CHECK(sv::arrow_style::for_length(1.0f) == sv::arrow_style());
    CHECK(sv::arrow_style::for_length(2.0f) == sv::arrow_style::for_shaft_radius(0.04f));

    auto const a = sv::arrow_style::for_shaft_radius(0.01f);
    auto const b = sv::arrow_style::for_shaft_radius(0.02f);

    // The tip's half-angle is what stays fixed across the two, which is what makes them read as the same arrow at two sizes.
    CHECK(near(a.head_radius / a.head_length, b.head_radius / b.head_length));
    CHECK(near(a.head_radius / a.head_length, 1.0f / 3.0f));
}

TEST("sv::quadric_set::add_arrow appends both primitives")
{
    auto set = sv::quadric_set();
    set.add_arrow(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0)));
    CHECK(set.primitive_count() == 2);

    // The bounds fold over both, so the batch spans the arrow.
    REQUIRE(set.bounds().has_value());
    CHECK(near(set.bounds().value().min[0], 0.0f));
    CHECK(near(set.bounds().value().max[0], 1.0f));

    // The three overloads are the same geometry where they agree, which is what keeps the sugar honest.
    auto explicit_style = sv::quadric_set();
    explicit_style.add_arrow(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0)), sv::arrow_style::for_length(1.0f));
    CHECK(explicit_style.hash() == set.hash());
}
