#include "../../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/query/query.hh>

// The point kernels and what derives from them: projection, distance, signed distance, contains, intersects.

namespace
{
// the comparison-only kernels evaluate at compile time, for every scalar
static_assert(tg::pos2i(5, 1).distance_sqr_to(tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(2, 2))) == 9);
static_assert(tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(2, 2)).contains(tg::pos2i(1, 2)));
static_assert(!tg::pos2i(3, 3).intersects(tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(2, 2))));
} // namespace

TEST("tg query - projecting a point")
{
    SECTION("onto a point")
    {
        CHECK(tg::pos3f(1, 2, 3).project_to(tg::pos3f(4, 5, 6)) == tg::pos3f(4, 5, 6));
    }

    SECTION("onto an aabb clamps per axis, and leaves an inside point alone")
    {
        auto const b = tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 1, 1));
        CHECK(tg::pos3f(2, 0.5f, -1).project_to(b) == tg::pos3f(1, 0.5f, 0));
        CHECK(tg::pos3f(0.25f, 0.5f, 0.75f).project_to(b) == tg::pos3f(0.25f, 0.5f, 0.75f));
    }

    SECTION("onto the linear objects clamps the foot to each one's parameter range")
    {
        auto const o = tg::pos2f(0, 0);
        auto const dir = tg::vec2f(2, 0);
        auto const p = tg::pos2f(-1, 1);

        CHECK(tgtest::approx(p.project_to(tg::line2f(o, dir)), tg::pos2f(-1, 0)));
        CHECK(tgtest::approx(p.project_to(tg::ray2f(o, dir)), o));
        CHECK(tgtest::approx(p.project_to(tg::segment2f(o, tg::pos2f(2, 0))), o));
        CHECK(tgtest::approx(tg::pos2f(5, 1).project_to(tg::segment2f(o, tg::pos2f(2, 0))), tg::pos2f(2, 0)));
        CHECK(tgtest::approx(tg::pos2f(1, 7).project_to(tg::segment2f(o, tg::pos2f(2, 0))), tg::pos2f(1, 0)));
    }

    SECTION("onto a plane drops the offset along the normal")
    {
        auto const pl = tg::plane3f(tg::vec3f(0, 0, 1), 2.0f);
        CHECK(tgtest::approx(tg::pos3f(1, 2, 7).project_to(pl), tg::pos3f(1, 2, 2)));
    }

    SECTION("onto a ball leaves an inside point alone; onto its surface moves it out")
    {
        auto const s = tg::sphere3f(tg::pos3f(0, 0, 0), 2.0f);
        CHECK(tg::pos3f(1, 0, 0).project_to(s) == tg::pos3f(1, 0, 0));
        CHECK(tgtest::approx(tg::pos3f(1, 0, 0).project_to(s.boundary()), tg::pos3f(2, 0, 0)));
        CHECK(tgtest::approx(tg::pos3f(0, 6, 0).project_to(s), tg::pos3f(0, 2, 0)));
    }

    SECTION("onto a triangle, in every region")
    {
        auto const t = tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(4, 0, 0), tg::pos3f(0, 4, 0));

        CHECK(tgtest::approx(tg::pos3f(1, 1, 5).project_to(t), tg::pos3f(1, 1, 0)));   // face
        CHECK(tgtest::approx(tg::pos3f(-1, -1, 3).project_to(t), tg::pos3f(0, 0, 0))); // vertex a
        CHECK(tgtest::approx(tg::pos3f(6, -1, 0).project_to(t), tg::pos3f(4, 0, 0)));  // vertex b
        CHECK(tgtest::approx(tg::pos3f(-1, 6, 0).project_to(t), tg::pos3f(0, 4, 0)));  // vertex c
        CHECK(tgtest::approx(tg::pos3f(2, -3, 1).project_to(t), tg::pos3f(2, 0, 0)));  // edge ab
        CHECK(tgtest::approx(tg::pos3f(-3, 2, 1).project_to(t), tg::pos3f(0, 2, 0)));  // edge ac
        CHECK(tgtest::approx(tg::pos3f(3, 3, 0).project_to(t), tg::pos3f(2, 2, 0)));   // edge bc
    }

    SECTION("onto a triangle in 2D, where the face is the point itself")
    {
        auto const t = tg::triangle2f(tg::pos2f(0, 0), tg::pos2f(4, 0), tg::pos2f(0, 4));
        CHECK(tgtest::approx(tg::pos2f(1, 1).project_to(t), tg::pos2f(1, 1)));
        CHECK(tgtest::approx(tg::pos2f(3, 3).project_to(t), tg::pos2f(2, 2)));
    }
}

TEST("tg query - distances derive from the projection")
{
    auto const t = tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(4, 0, 0), tg::pos3f(0, 4, 0));
    auto const p = tg::pos3f(1, 1, 5);

    CHECK(tgtest::approx(p.distance_sqr_to(t), 25.0f));
    CHECK(tgtest::approx(p.distance_to(t), 5.0f));
    CHECK(tgtest::approx(t.distance_to(p), 5.0f));
    CHECK(tgtest::approx(p.distance_to(tg::pos3f(1, 1, 2)), 3.0f));
}

TEST("tg query - signed distance is negative inside")
{
    auto const s = tg::sphere3f(tg::pos3f(0, 0, 0), 2.0f);
    CHECK(tgtest::approx(tg::pos3f(1, 0, 0).signed_distance_to(s), -1.0f));
    CHECK(tgtest::approx(tg::pos3f(0, 0, 5).signed_distance_to(s.boundary()), 3.0f));

    auto const pl = tg::plane3f(tg::vec3f(0, 0, 1), 2.0f);
    CHECK(tgtest::approx(tg::pos3f(0, 0, 1).signed_distance_to(pl), -1.0f));
}

TEST("tg query - containment and intersection of a point")
{
    auto const s = tg::sphere3f(tg::pos3f(0, 0, 0), 2.0f);

    CHECK(s.contains(tg::pos3f(1, 1, 1)));
    CHECK(!s.contains(tg::pos3f(2, 2, 2)));
    CHECK(tg::pos3f(1, 1, 1).intersects(s));
    CHECK(s.intersects(tg::pos3f(1, 1, 1)));

    // a point inside the ball is not on its surface
    CHECK(!s.boundary().contains(tg::pos3f(1, 0, 0)));
    CHECK(s.boundary().contains(tg::pos3f(2, 0, 0)));

    CHECK(tg::pos3f(1, 2, 3).contains(tg::pos3f(1, 2, 3)));
    CHECK(!tg::pos3f(1, 2, 3).intersects(tg::pos3f(1, 2, 4)));
}

TEST("tg query - a point no direction leads away from still projects")
{
    SECTION("a ball's center is its radius from its surface")
    {
        auto const ball = tg::sphere3d(tg::pos3d(0, 0, 0), 2.0);
        CHECK(tgtest::approx(tg::pos3d(0, 0, 0).distance_to(ball.boundary()), 2.0));
        CHECK(tgtest::approx(tg::pos2d(1, 1).distance_to(tg::sphere2d(tg::pos2d(1, 1), 3.0).boundary()), 3.0));
    }

    SECTION("a point on the axis goes out perpendicular to it")
    {
        auto const axis = tg::segment3d(tg::pos3d(0, 0, 0), tg::pos3d(0, 0, 4));
        auto const on_axis = tg::pos3d(0, 0, 2);
        auto const cyl = tg::cylinder3d(axis, 1.0);
        CHECK(tgtest::approx(on_axis.distance_to(cyl.boundary()), 1.0));
        CHECK(tgtest::approx(on_axis.distance_to(cyl.mantle()), 1.0));
        CHECK(tgtest::approx(on_axis.project_to(cyl.mantle()).data[2], 2.0));
        // beyond the tube's extent the axis point lands on the rim
        CHECK(tgtest::approx(tg::pos3d(0, 0, 6).distance_to(cyl.mantle()), tg::sqrt(5.0)));

        auto const cap = tg::capsule3d(axis, 1.0);
        CHECK(tgtest::approx(on_axis.distance_to(cap.boundary()), 1.0));
        CHECK(tgtest::approx(axis.pos1.distance_to(cap.boundary()), 1.0));
        auto const flat = tg::capsule2d(tg::segment2d(tg::pos2d(0, 0), tg::pos2d(4, 0)), 1.0);
        CHECK(tgtest::approx(tg::pos2d(2, 0).distance_to(flat.boundary()), 1.0));

        auto const inf = tg::inf_cylinder3d(tg::line3d(tg::pos3d(1, 1, 0), tg::vec3d(0, 0, 1)), 2.0);
        CHECK(tgtest::approx(tg::pos3d(1, 1, 7).distance_to(inf.boundary()), 2.0));
        CHECK(tgtest::approx(tg::pos3d(1, 1, 7).project_to(inf.boundary()).data[2], 7.0));
        auto const band = tg::inf_cylinder<2, double>(tg::line2d(tg::pos2d(0, 0), tg::vec2d(1, 0)), 1.0);
        CHECK(tgtest::approx(tg::pos2d(3, 0).distance_to(band.boundary()), 1.0));
    }
}
