#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::aabb3f>, "aabb should be trivially copyable");

TEST("tg aabb - construction")
{
    SECTION("default is degenerate at the origin")
    {
        tg::aabb3f b;
        CHECK(b.min == tg::pos3f::zero);
        CHECK(b.max == tg::pos3f::zero);
    }

    SECTION("explicit ctor stores min/max")
    {
        auto const lo = tg::pos3f(0, 0, 0);
        auto const hi = tg::pos3f(1, 2, 3);
        tg::aabb3f const b(lo, hi);
        CHECK(b.min == lo);
        CHECK(b.max == hi);
    }
}

TEST("tg aabb - equality")
{
    auto const b = tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(1, 1));
    CHECK(b == tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(1, 1)));
    CHECK(b != tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(2, 1)));
}
