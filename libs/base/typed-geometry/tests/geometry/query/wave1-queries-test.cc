#include "../../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/query/query.hh>

// The wave-1 objects in the query layer: half-spaces and planes against anything with a support, the oriented box,
// and the faces of an aabb.

namespace
{
static_assert(tg::has_intersects<tg::halfspace3f, tg::sphere3f>);
static_assert(tg::has_intersects<tg::triangle3f, tg::plane3f>, "the plane kernel serves either argument order");
static_assert(tg::has_contains<tg::halfspace3f, tg::aabb3f>);
static_assert(tg::has_distance_sqr_to<tg::box3f, tg::sphere3f>, "a box has a support, so GJK serves it");
static_assert(tg::has_separation_from<tg::box3f, tg::aabb3f>);
static_assert(!tg::has_project_to<tg::pos3f, tg::box3f>, "clamping in box coordinates is not the nearest point");
} // namespace

TEST("tg query - a half-space against objects with a support")
{
    auto const h = tg::halfspace3f(tg::vec3f(0, 0, 1), 2.0f); // z <= 2

    CHECK(h.intersects(tg::sphere3f(tg::pos3f(0, 0, 2.5f), 1.0f)));
    CHECK(!h.intersects(tg::sphere3f(tg::pos3f(0, 0, 3.5f), 1.0f)));
    CHECK(tg::sphere3f(tg::pos3f(0, 0, 2.5f), 1.0f).intersects(h));

    CHECK(h.contains(tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 1, 2))));
    CHECK(!h.contains(tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 1, 2.5f))));

    CHECK(h.contains(tg::pos3f(5, 5, -100)));
    CHECK(tgtest::approx(tg::pos3f(1, 1, 5).project_to(h), tg::pos3f(1, 1, 2)));
    CHECK(tg::pos3f(1, 1, 0).project_to(h) == tg::pos3f(1, 1, 0));
    CHECK(tgtest::approx(tg::pos3f(1, 1, 0).signed_distance_to(h), -2.0f));
}

TEST("tg query - a plane meets an object that reaches both of its sides")
{
    auto const pl = tg::plane3f(tg::vec3f(0, 0, 1), 0.0f);

    CHECK(pl.intersects(tg::segment3f(tg::pos3f(0, 0, -1), tg::pos3f(0, 0, 1))));
    CHECK(!pl.intersects(tg::segment3f(tg::pos3f(0, 0, 1), tg::pos3f(0, 0, 2))));
    CHECK(tg::triangle3f(tg::pos3f(0, 0, -1), tg::pos3f(1, 0, 1), tg::pos3f(0, 1, 1)).intersects(pl));
}

TEST("tg query - an oriented box")
{
    // the unit cube rotated 45 degrees about z: its corners reach sqrt(2) along x
    auto const a = tg::aabb3d(tg::pos3d(-1, -1, -1), tg::pos3d(1, 1, 1));
    auto const t = tg::rigid_transform3d::make_rotation(tg::quat_d::make_rotation_z(tg::angle_d::make_from_degree(45)));
    auto const b = a.transformed(t);

    CHECK(b.contains(tg::pos3d(1.4, 0, 0)));
    CHECK(!b.contains(tg::pos3d(1, 1, 0)));
    CHECK(tgtest::approx(tg::pos3d(3, 0, 0).distance_to(b), 3 - tg::sqrt(2.0), 1e-9));
    CHECK(b.intersects(tg::sphere3d(tg::pos3d(2, 0, 0), 0.6)));
    CHECK(!b.intersects(tg::sphere3d(tg::pos3d(2, 0, 0), 0.5)));
}

TEST("tg query - the faces of an aabb")
{
    auto const s = tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(4, 4, 4)).boundary();

    // inside, the nearest face pulls the point onto itself; outside, it is the solid's clamp
    CHECK(tg::pos3f(1, 2, 2).project_to(s) == tg::pos3f(0, 2, 2));
    CHECK(tg::pos3f(3.5f, 2, 2).project_to(s) == tg::pos3f(4, 2, 2));
    CHECK(tg::pos3f(9, 2, 2).project_to(s) == tg::pos3f(4, 2, 2));

    CHECK(tgtest::approx(tg::pos3f(1, 2, 2).distance_to(s), 1.0f));
    CHECK(!s.contains(tg::pos3f(1, 2, 2)));
    CHECK(s.contains(tg::pos3f(0, 2, 2)));
    CHECK(tg::pos3f(4, 1, 1).intersects(s));
}
