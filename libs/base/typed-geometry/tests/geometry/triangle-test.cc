#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/triangle.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::triangle3f>, "triangle should be trivially copyable");

TEST("tg triangle - construction")
{
    SECTION("default has all vertices at the origin")
    {
        tg::triangle3f t;
        CHECK(t.pos0 == tg::pos3f::zero);
        CHECK(t.pos1 == tg::pos3f::zero);
        CHECK(t.pos2 == tg::pos3f::zero);
    }

    SECTION("explicit ctor stores the three vertices")
    {
        auto const a = tg::pos3f(0, 0, 0);
        auto const b = tg::pos3f(1, 0, 0);
        auto const c = tg::pos3f(0, 1, 0);
        tg::triangle3f const t(a, b, c);
        CHECK(t.pos0 == a);
        CHECK(t.pos1 == b);
        CHECK(t.pos2 == c);
    }
}

TEST("tg triangle - equality")
{
    auto const a = tg::pos2f(0, 0);
    auto const b = tg::pos2f(1, 0);
    auto const c = tg::pos2f(0, 1);
    tg::triangle2f const t(a, b, c);
    CHECK(t == tg::triangle2f(a, b, c));
    CHECK(t != tg::triangle2f(a, c, b));
}
