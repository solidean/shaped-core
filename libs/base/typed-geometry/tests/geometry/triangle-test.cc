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

TEST("tg triangle - a nearly collinear triangle has a finite, non-negative area")
{
    // Lagrange's identity cancels here and once went NaN; the cross product's length cannot
    auto const t = tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(1.1f, 2.3f, 3.7f),
                                  tg::pos3f(1.1f * 10.0f, 2.3f * 10.0f, 3.7f * 10.0f));
    auto const a = t.area();
    CHECK(a >= 0.0f);
    CHECK(a < 1e-2f);
    CHECK(tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(3, 0, 0), tg::pos3f(0, 4, 0)).area() == 6.0f);
}
