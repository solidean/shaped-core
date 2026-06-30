#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/ray.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::ray3f>, "ray should be trivially copyable");

TEST("tg ray - construction")
{
    SECTION("default has origin and dir zero")
    {
        tg::ray3f r;
        CHECK(r.origin == tg::pos3f::zero);
        CHECK(r.dir == tg::vec3f::zero);
    }

    SECTION("explicit ctor stores origin and dir")
    {
        auto const o = tg::pos3f(1, 2, 3);
        auto const d = tg::vec3f(0, 0, 1);
        tg::ray3f const r(o, d);
        CHECK(r.origin == o);
        CHECK(r.dir == d);
    }
}

TEST("tg ray - equality")
{
    auto const o = tg::pos2f(0, 0);
    auto const d = tg::vec2f(1, 0);
    tg::ray2f const r(o, d);
    CHECK(r == tg::ray2f(o, d));
    CHECK(r != tg::ray2f(o, tg::vec2f(0, 1)));
}
