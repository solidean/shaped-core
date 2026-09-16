#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/scene/quadric.hh>
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

TEST("sv::append_capsule writes a cylinder and two caps")
{
    auto prims = cc::vector<sv::quadric_primitive>();
    sv::append_capsule(prims, tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 0, 4)), 1.0f);

    REQUIRE(prims.size() == 3);

    // The cap the flat-capped form leaves off: a point beyond the segment's end is on the capsule and not on the cylinder.
    auto const beyond = tg::pos3f(0, 0, 4.5f);
    CHECK(!prims[0].admits(beyond));
    CHECK(prims[2].admits(beyond));

    // The union's box reaches a full radius past each end, where the cylinder alone stops at the end.
    CHECK(near(prims[0].bounds.max[2], 4.0f));
    CHECK(near(prims[2].bounds.max[2], 5.0f));
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

    // Straight down the axis from outside: the cap plane is crossed, but it is not a surface, so the ray reaches the
    // inside of the far wall instead.
    auto const hit = sv::intersect(p, ray_from(tg::pos3f(0.1f, 0, -2), tg::vec3f(0, 0, 1)));
    REQUIRE(hit.has_value());

    auto const at = tg::pos3f(0.1f, 0, -2) + tg::vec3f(0, 0, 1) * hit.value().t;
    CHECK(at[2] > 0.0f); // past the near cap plane, so nothing was drawn there
}

TEST("sv::intersect draws the cap of a capped cylinder")
{
    auto const p
        = sv::quadric_primitive::create_cylinder(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 0, 4)), 1.0f, true);
    CHECK(p.emits_clip_surface());

    // The same ray now stops on the flat end at z = 0, which is the CLIPPER's own surface rather than the cylinder's.
    auto const hit = sv::intersect(p, ray_from(tg::pos3f(0.1f, 0, -2), tg::vec3f(0, 0, 1)));
    REQUIRE(hit.has_value());
    CHECK(near(hit.value().t, 2.0f));

    // And its normal is the cap's, pointing back along the axis at the ray rather than radially outward.
    CHECK(near(hit.value().normal, tg::vec3f(0, 0, -1)));

    // A ray down the axis but OUTSIDE the cylinder's radius misses entirely: the cap is only a surface where it lies
    // inside the other quadric, which is the whole interval test.
    CHECK(!sv::intersect(p, ray_from(tg::pos3f(3, 0, -2), tg::vec3f(0, 0, 1))).has_value());
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
