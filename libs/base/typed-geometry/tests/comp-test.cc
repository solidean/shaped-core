#include <nexus/test.hh>
#include <typed-geometry/linalg/comp.hh>
#include <typed-geometry/linalg/comp_ops.hh>

#include <type_traits>

// comp must stay a trivially copyable value type (the zero-init default member initializer
// only affects the default constructor; copy/move stay trivial).
static_assert(std::is_trivially_copyable_v<tg::comp3f>, "comp should be trivially copyable");
static_assert(std::is_trivially_copyable_v<tg::comp4i>, "comp should be trivially copyable");

TEST("tg comp - construction")
{
    SECTION("default zero-inits")
    {
        tg::comp3f c;
        CHECK(c[0] == 0);
        CHECK(c[1] == 0);
        CHECK(c[2] == 0);
    }

    SECTION("splat")
    {
        auto const c = tg::comp3f(2.0f);
        CHECK(c[0] == 2);
        CHECK(c[1] == 2);
        CHECK(c[2] == 2);
    }

    SECTION("per-dimension")
    {
        auto const c2 = tg::comp2i(1, 2);
        CHECK(c2[0] == 1);
        CHECK(c2[1] == 2);

        auto const c3 = tg::comp3i(1, 2, 3);
        CHECK(c3[2] == 3);

        auto const c4 = tg::comp4i(1, 2, 3, 4);
        CHECK(c4[3] == 4);
    }

    SECTION("initializer list")
    {
        auto const c = tg::comp3i({5, 6, 7});
        CHECK(c[0] == 5);
        CHECK(c[1] == 6);
        CHECK(c[2] == 7);
    }

    SECTION("make_from_values")
    {
        auto const c = tg::comp3f::make_from_values(1, 2, 3);
        CHECK(c[0] == 1);
        CHECK(c[1] == 2);
        CHECK(c[2] == 3);
    }

    SECTION("zero")
    {
        CHECK(tg::comp3f::zero == tg::comp3f(0, 0, 0));
    }
}

TEST("tg comp - access and equality")
{
    auto c = tg::comp3i(1, 2, 3);

    SECTION("data member is the raw storage")
    {
        CHECK(c.data[0] == 1);
        c.data[1] = 20;
        CHECK(c[1] == 20);
    }

    SECTION("mutating subscript")
    {
        c[0] = 10;
        CHECK(c[0] == 10);
    }

    SECTION("equality")
    {
        CHECK(c == tg::comp3i(1, 2, 3));
        CHECK(c != tg::comp3i(1, 2, 4));
    }

    SECTION("out-of-range subscript asserts")
    {
        CHECK_ASSERTS(c[-1]);
        CHECK_ASSERTS(c[3]);
    }
}

TEST("tg comp - component-wise arithmetic")
{
    auto const a = tg::comp3i(1, 2, 3);
    auto const b = tg::comp3i(10, 20, 30);

    SECTION("comp-comp is element-wise (incl. Hadamard * and /)")
    {
        CHECK(a + b == tg::comp3i(11, 22, 33));
        CHECK(b - a == tg::comp3i(9, 18, 27));
        CHECK(a * b == tg::comp3i(10, 40, 90));
        CHECK(b / a == tg::comp3i(10, 10, 10));
        CHECK(-a == tg::comp3i(-1, -2, -3));
    }

    SECTION("scalar broadcasts on both sides")
    {
        CHECK(a + 10 == tg::comp3i(11, 12, 13));
        CHECK(10 + a == tg::comp3i(11, 12, 13));
        CHECK(a * 2 == tg::comp3i(2, 4, 6));
        CHECK(2 * a == tg::comp3i(2, 4, 6));
        CHECK(b / 10 == tg::comp3i(1, 2, 3));
        CHECK(7 - a == tg::comp3i(6, 5, 4));
        CHECK(tg::comp3i(12, 6, 4) == tg::comp3i(12, 12, 12) / tg::comp3i(1, 2, 3));
        CHECK(12 / tg::comp3i(1, 2, 3) == tg::comp3i(12, 6, 4));
    }

    SECTION("compound assignment, comp and scalar rhs")
    {
        auto c = a;
        c += b;
        CHECK(c == tg::comp3i(11, 22, 33));
        c -= b;
        CHECK(c == a);
        c *= 2;
        CHECK(c == tg::comp3i(2, 4, 6));
        c /= 2;
        CHECK(c == a);
        c *= tg::comp3i(2, 3, 4);
        CHECK(c == tg::comp3i(2, 6, 12));
    }
}

TEST("tg comp - min / max")
{
    auto const a = tg::comp3i(1, 9, 3);
    auto const b = tg::comp3i(5, 2, 7);

    CHECK(tg::min(a, b) == tg::comp3i(1, 2, 3));
    CHECK(tg::max(a, b) == tg::comp3i(5, 9, 7));
    CHECK(tg::min(a, 4) == tg::comp3i(1, 4, 3));
    CHECK(tg::max(a, 4) == tg::comp3i(4, 9, 4));
}
