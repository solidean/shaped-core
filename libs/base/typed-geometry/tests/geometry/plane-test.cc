#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/plane.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::plane3f>, "plane should be trivially copyable");

TEST("tg plane - construction")
{
    SECTION("default has zero normal and zero distance")
    {
        tg::plane3f p;
        CHECK(p.normal == tg::vec3f::zero);
        CHECK(p.dist == 0);
    }

    SECTION("explicit ctor stores normal and distance")
    {
        auto const n = tg::vec3f(0, 0, 1);
        tg::plane3f const p(n, 5.0f);
        CHECK(p.normal == n);
        CHECK(p.dist == 5.0f);
    }
}

TEST("tg plane - equality")
{
    auto const n = tg::vec2f(1, 0);
    tg::plane2f const p(n, 3.0f);
    CHECK(p == tg::plane2f(n, 3.0f));
    CHECK(p != tg::plane2f(n, 4.0f));
}
