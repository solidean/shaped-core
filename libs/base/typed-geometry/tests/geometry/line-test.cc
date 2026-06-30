#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/line.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::line3f>, "line should be trivially copyable");

TEST("tg line - construction")
{
    SECTION("default has origin and dir zero")
    {
        tg::line3f l;
        CHECK(l.origin == tg::pos3f::zero);
        CHECK(l.dir == tg::vec3f::zero);
    }

    SECTION("explicit ctor stores origin and dir")
    {
        auto const o = tg::pos3f(0, 0, 0);
        auto const d = tg::vec3f(1, 0, 0);
        tg::line3f const l(o, d);
        CHECK(l.origin == o);
        CHECK(l.dir == d);
    }
}

TEST("tg line - equality")
{
    auto const o = tg::pos2f(2, 2);
    auto const d = tg::vec2f(1, 0);
    tg::line2f const l(o, d);
    CHECK(l == tg::line2f(o, d));
    CHECK(l != tg::line2f(tg::pos2f(0, 0), d));
}
