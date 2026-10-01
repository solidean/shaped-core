#include "../../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/query/query.hh>

#include <type_traits>

// The parametric and constructive verbs: where a line, ray or segment meets each wave-1 object, and the overlaps that
// are primitives of their own.

namespace
{
static_assert(tg::has_intersection_parameter_with<tg::ray3f, tg::sphere3f>);
static_assert(tg::has_intersection_with<tg::sphere3f, tg::ray3f>, "the linear object may come second");
static_assert(!tg::has_intersection_with<tg::ray3f, tg::halfspace3f>, "a ray inside a half-space can be unbounded");
static_assert(tg::has_intersects<tg::ray3f, tg::triangle3f>, "intersects derives from the parameters");
static_assert(!tg::has_intersection_with<tg::triangle3f, tg::triangle3f>, "their overlap has no type");

auto const unit_ball = tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f);
} // namespace

TEST("tg intersection - a ray against a ball and its surface")
{
    auto const r = tg::ray3f(tg::pos3f(-3, 0, 0), tg::vec3f(1, 0, 0));

    auto const in = r.intersection_parameter_with(unit_ball);
    REQUIRE(in.has_value());
    CHECK(tgtest::approx(in.value().start, 2.0f));
    CHECK(tgtest::approx(in.value().end, 4.0f));

    auto const hits = r.intersection_parameter_with(unit_ball.boundary());
    REQUIRE(hits.size() == 2);
    CHECK(tgtest::approx(hits.first(), 2.0f));
    CHECK(tgtest::approx(hits.last(), 4.0f));

    SECTION("from inside, the solid starts at the origin and the surface has one crossing")
    {
        auto const inside = tg::ray3f(tg::pos3f(0, 0, 0), tg::vec3f(0, 1, 0));
        CHECK(inside.closest_intersection_parameter_with(unit_ball).value() == 0.0f);
        CHECK(tgtest::approx(inside.closest_intersection_parameter_with(unit_ball.boundary()).value(), 1.0f));
        CHECK(inside.intersection_parameter_with(unit_ball.boundary()).size() == 1);
    }

    SECTION("a miss")
    {
        auto const away = tg::ray3f(tg::pos3f(-3, 2, 0), tg::vec3f(1, 0, 0));
        CHECK(!away.intersects(unit_ball));
        CHECK(!away.closest_intersection_parameter_with(unit_ball).has_value());
    }

    SECTION("as geometry: the segment inside the ball, the points on its surface")
    {
        auto const seg = r.intersection_with(unit_ball);
        REQUIRE(seg.has_value());
        CHECK(tgtest::approx(seg.value().pos0, tg::pos3f(-1, 0, 0)));
        CHECK(tgtest::approx(seg.value().pos1, tg::pos3f(1, 0, 0)));
        CHECK(tgtest::approx(r.intersection_with(unit_ball.boundary()).last(), tg::pos3f(1, 0, 0)));
    }
}

TEST("tg intersection - linear objects against planes, boxes and triangles")
{
    SECTION("a segment crosses a plane at one point")
    {
        auto const pl = tg::plane3f(tg::vec3f(0, 0, 1), 1.0f);
        auto const s = tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 0, 4));
        CHECK(tgtest::approx(s.intersection_with(pl).value(), tg::pos3f(0, 0, 1)));
        CHECK(!tg::segment3f(tg::pos3f(0, 0, 2), tg::pos3f(0, 0, 4)).intersects(pl));
    }

    SECTION("a line through a half-space is unbounded on one side")
    {
        auto const h = tg::halfspace3f(tg::vec3f(0, 0, 1), 1.0f);
        auto const in = tg::line3f(tg::pos3f(0, 0, 0), tg::vec3f(0, 0, 1)).intersection_parameter_with(h);
        REQUIRE(in.has_value());
        CHECK(tgtest::approx(in.value().end, 1.0f));
        CHECK(in.value().start < -1e30f);
    }

    SECTION("slabs: an aabb, its faces, and a rotated box")
    {
        auto const b = tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(2, 2, 2));
        auto const r = tg::ray3f(tg::pos3f(-1, 1, 1), tg::vec3f(1, 0, 0));
        CHECK(tgtest::approx(r.closest_intersection_parameter_with(b).value(), 1.0f));
        auto const faces = r.intersection_parameter_with(b.boundary());
        REQUIRE(faces.size() == 2);
        CHECK(tgtest::approx(faces.last(), 3.0f));

        // parallel to a slab and outside it: an exact miss, not a division by zero
        CHECK(!tg::ray3f(tg::pos3f(-1, 5, 1), tg::vec3f(1, 0, 0)).intersects(b));

        auto const t
            = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(45)));
        auto const rotated = tg::aabb3f(tg::pos3f(-1, -1, -1), tg::pos3f(1, 1, 1)).transformed(t);
        auto const hit = tg::ray3f(tg::pos3f(-5, 0, 0), tg::vec3f(1, 0, 0)).intersection_parameter_with(rotated);
        REQUIRE(hit.has_value());
        CHECK(tgtest::approx(hit.value().start, 5 - tg::sqrt(2.0f), 1e-4f));
    }

    SECTION("a triangle in 3D is crossed once, in 2D passed through")
    {
        auto const t3 = tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(2, 0, 0), tg::pos3f(0, 2, 0));
        auto const down = tg::ray3f(tg::pos3f(0.5f, 0.5f, 3), tg::vec3f(0, 0, -1));
        CHECK(tgtest::approx(down.closest_intersection_parameter_with(t3).value(), 3.0f));
        CHECK(!tg::ray3f(tg::pos3f(1.5f, 1.5f, 3), tg::vec3f(0, 0, -1)).intersects(t3));

        auto const t2 = tg::triangle2f(tg::pos2f(0, 0), tg::pos2f(4, 0), tg::pos2f(0, 4));
        auto const in = tg::line2f(tg::pos2f(-1, 1), tg::vec2f(1, 0)).intersection_parameter_with(t2);
        REQUIRE(in.has_value());
        CHECK(tgtest::approx(in.value().start, 1.0f));
        CHECK(tgtest::approx(in.value().end, 4.0f));
    }

    SECTION("two segments in the plane")
    {
        auto const a = tg::segment2f(tg::pos2f(0, 0), tg::pos2f(4, 4));
        auto const b = tg::segment2f(tg::pos2f(0, 4), tg::pos2f(4, 0));
        CHECK(tgtest::approx(a.intersection_parameter_with(b).first(), 0.5f));
        CHECK(tgtest::approx(a.intersection_with(b).value(), tg::pos2f(2, 2)));
        CHECK(!a.intersects(tg::segment2f(tg::pos2f(3, 0), tg::pos2f(4, 0))));
    }

    SECTION("an ellipsoid, through its own frame")
    {
        auto const e = tg::ellipsoid3f(tg::pos3f(0, 0, 0), tg::vec3f(4, 0, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, 1));
        auto const r = tg::ray3f(tg::pos3f(-10, 0, 0), tg::vec3f(1, 0, 0));
        CHECK(tgtest::approx(r.closest_intersection_parameter_with(e).value(), 6.0f));
        CHECK(tgtest::approx(r.intersection_parameter_with(e.boundary()).last(), 14.0f));
    }
}

TEST("tg intersection - overlaps that are primitives")
{
    SECTION("two boxes overlap in a box")
    {
        auto const a = tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(4, 4));
        auto const b = tg::aabb2i(tg::pos2i(2, 1), tg::pos2i(6, 3));
        CHECK(a.intersection_with(b).value() == tg::aabb2i(tg::pos2i(2, 1), tg::pos2i(4, 3)));
        CHECK(!a.intersection_with(tg::aabb2i(tg::pos2i(5, 5), tg::pos2i(6, 6))).has_value());
    }

    SECTION("two planes meet in a line on both")
    {
        auto const a = tg::plane3f(tg::vec3f(1, 0, 0), 2.0f);
        auto const b = tg::plane3f(tg::vec3f(0, 1, 0), 3.0f);
        auto const l = a.intersection_with(b).value();
        CHECK(tgtest::approx(l.origin, tg::pos3f(2, 3, 0)));
        CHECK(tgtest::approx(l.dir, tg::vec3f(0, 0, 1)));
    }

    SECTION("a triangle crosses a plane in a segment")
    {
        auto const t = tg::triangle3f(tg::pos3f(0, 0, -1), tg::pos3f(2, 0, 1), tg::pos3f(0, 2, 1));
        auto const s = t.intersection_with(tg::plane3f(tg::vec3f(0, 0, 1), 0.0f));
        REQUIRE(s.has_value());
        CHECK(tgtest::approx(s.value().length(), tg::sqrt(2.0f)));
    }

    SECTION("a ball cut by a plane is a disk; two sphere surfaces meet in a circle")
    {
        auto const d = tg::sphere3f(tg::pos3f(0, 0, 0), 2.0f).intersection_with(tg::plane3f(tg::vec3f(0, 0, 1), 1.0f));
        REQUIRE(d.has_value());
        static_assert(std::is_same_v<decltype(d), cc::optional<tg::disk3f> const>);
        CHECK(tgtest::approx(d.value().radius, tg::sqrt(3.0f)));
        CHECK(tgtest::approx(d.value().center, tg::pos3f(0, 0, 1)));

        auto const a = tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f).boundary();
        auto const b = tg::sphere3f(tg::pos3f(1, 0, 0), 1.0f).boundary();
        auto const c = a.intersection_with(b);
        REQUIRE(c.has_value());
        CHECK(tgtest::approx(c.value().center, tg::pos3f(0.5f, 0, 0)));
        CHECK(tgtest::approx(c.value().radius, tg::sqrt(0.75f)));
        CHECK(!a.intersection_with(tg::sphere3f(tg::pos3f(3, 0, 0), 1.0f).boundary()).has_value());
    }
}
