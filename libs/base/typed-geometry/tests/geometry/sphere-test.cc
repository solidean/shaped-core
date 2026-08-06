#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/sphere.hh>
#include <typed-geometry/geometry/traits.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::sphere3f>, "sphere should be trivially copyable");

namespace
{
// a sphere is the SURFACE, so it is codimension 1 — a future tg::ball will reuse the encoding
static_assert(tg::traits::intrinsic_dim<tg::sphere3f> == 2);
static_assert(tg::traits::ambient_dim<tg::sphere3f> == 3);
static_assert(tg::traits::is_finite<tg::sphere3f>);
static_assert(tg::traits::intrinsic_dim<tg::sphere2f> == 1, "a 2D sphere is a circle");
} // namespace

TEST("tg sphere - construction")
{
    SECTION("default is a degenerate point sphere at the origin")
    {
        auto const s = tg::sphere3f();
        CHECK(s.center == tg::pos3f::zero);
        CHECK(s.radius == 0.0f);
    }

    SECTION("explicit construction keeps center and radius")
    {
        auto const s = tg::sphere3f(tg::pos3f(1, 2, 3), 4.0f);
        CHECK(s.center == tg::pos3f(1, 2, 3));
        CHECK(s.radius == 4.0f);
    }

    SECTION("equality is member-wise")
    {
        CHECK(tg::sphere3f(tg::pos3f(1, 0, 0), 2.0f) == tg::sphere3f(tg::pos3f(1, 0, 0), 2.0f));
        CHECK(tg::sphere3f(tg::pos3f(1, 0, 0), 2.0f) != tg::sphere3f(tg::pos3f(1, 0, 0), 3.0f));
    }
}
