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
    nx::expect_warning("sphere's center", {.domain = "tg"});
    auto const s = tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f).boundary();
    // the direction is 0 / 0, so the answer is NaN rather than a point
    CHECK(s.center.project_to(s) != s.center);
}
#endif
