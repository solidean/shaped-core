#include <nexus/test.hh>
#include <typed-geometry/geometry/query/query.hh>

// A special case returns what the formula gives, whether or not the check is compiled in.
// With SC_CHECK_GEOMETRY_SPECIAL_CASES it also logs a warning, which a test feeding one on purpose declares.

TEST("tg query - a special case still answers")
{
#if TG_CHECK_SPECIAL_CASES
    nx::expect_warning("zero direction", {.domain = "tg"});
#endif

    // a zero-length segment: the foot parameter is 0 / 0, and NaN propagates rather than asserting
    auto const s = tg::segment2f(tg::pos2f(1, 1), tg::pos2f(1, 1));
    auto const q = tg::pos2f(3, 1).project_to(s);
    CHECK(q != tg::pos2f(3, 1));
}

#if TG_CHECK_SPECIAL_CASES
TEST("tg query - a special case names itself")
{
    nx::expect_warning("triangle's plane", {.domain = "tg"});
    auto const t = tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(2, 0, 0), tg::pos3f(0, 2, 0));
    // a ray lying in the triangle's plane crosses nothing, by the kernel's choice rather than its geometry
    CHECK(!tg::ray3f(tg::pos3f(-1, 0.5f, 0), tg::vec3f(1, 0, 0)).intersects(t));
}
#endif

TEST("tg query - a linear object lying in a plane meets it at its own origin")
{
#if TG_CHECK_SPECIAL_CASES
    nx::expect_warning("in a plane", {.domain = "tg"});
#endif

    auto const pl = tg::plane3d(tg::vec3d(0, 0, 1), 5.0);
    auto const r = tg::ray3d(tg::pos3d(1, 2, 5), tg::vec3d(1, 0, 0));
    auto const h = r.intersection_parameter_with(pl);
    REQUIRE(h.size() == 1);
    CHECK(h.first() == 0.0);
    CHECK(r.intersects(pl));
    CHECK(tg::line3d(tg::pos3d(1, 2, 5), tg::vec3d(0, 1, 0)).intersects(pl));
}

TEST("tg query - two collinear lines in the plane do not cross")
{
#if TG_CHECK_SPECIAL_CASES
    nx::expect_warning("collinear", {.domain = "tg"});
#endif

    auto const a = tg::segment2d(tg::pos2d(0, 0), tg::pos2d(2, 0));
    auto const b = tg::segment2d(tg::pos2d(1, 0), tg::pos2d(3, 0));
    CHECK(a.intersection_parameter_with(b).is_empty());
}

TEST("tg query - two coincident planes or spheres give no overlap")
{
#if TG_CHECK_SPECIAL_CASES
    nx::expect_warning("coincident planes", {.domain = "tg"});
    nx::expect_warning("coincident spheres", {.domain = "tg"});
#endif

    auto const pl = tg::plane3d(tg::vec3d(0, 0, 1), 2.0);
    CHECK(!pl.intersection_with(pl).has_value());
    CHECK(!pl.intersection_with(tg::plane3d(tg::vec3d(0, 0, -1), -2.0)).has_value());
    auto const s = tg::sphere3d(tg::pos3d(1, 1, 1), 2.0).boundary();
    CHECK(!s.intersection_with(s).has_value());
}
