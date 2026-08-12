#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <compare>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::angle_f>, "angle should be trivially copyable");

TEST("tg angle - construction and conversion")
{
    SECTION("default is zero")
    {
        tg::angle_f a;
        CHECK(a.radians() == 0);
        CHECK(a.degree() == 0);
    }

    SECTION("radians round-trip")
    {
        auto const a = tg::angle_f::make_from_radians(tg::pi<float> / 2);
        CHECK(tgtest::approx(a.radians(), tg::pi<float> / 2));
        CHECK(tgtest::approx(a.degree(), 90.0f));
    }

    SECTION("degree round-trip")
    {
        auto const a = tg::angle_d::make_from_degree(180);
        CHECK(tgtest::approx(a.radians(), tg::pi<double>));
        CHECK(tgtest::approx(a.degree(), 180.0));
    }
}

TEST("tg angle - arithmetic")
{
    auto const a = tg::angle_f::make_from_degree(30);
    auto const b = tg::angle_f::make_from_degree(60);

    CHECK(tgtest::approx((a + b).degree(), 90.0f));
    CHECK(tgtest::approx((b - a).degree(), 30.0f));
    CHECK(tgtest::approx((-a).degree(), -30.0f));
    CHECK(tgtest::approx((a * 3.0f).degree(), 90.0f));
    CHECK(tgtest::approx((3.0f * a).degree(), 90.0f));
    CHECK(tgtest::approx((b / 2.0f).degree(), 30.0f));

    SECTION("compound assignment matches the binary forms")
    {
        auto c = a;
        c += b;
        CHECK(tgtest::approx(c.degree(), 90.0f));
        c -= b;
        CHECK(tgtest::approx(c.degree(), 30.0f));
        c *= 3.0f;
        CHECK(tgtest::approx(c.degree(), 90.0f));
        c /= 3.0f;
        CHECK(tgtest::approx(c.degree(), 30.0f));
    }
}

TEST("tg angle - ordering")
{
    auto const a = tg::angle_f::make_from_degree(30);
    auto const b = tg::angle_f::make_from_degree(60);

    SECTION("orders by the underlying value")
    {
        CHECK(a < b);
        CHECK(b > a);
        CHECK(a <= a);
        CHECK(a >= a);
        CHECK(!(a > b));
    }

    SECTION("a full turn is greater, not equal — angle never wraps")
    {
        // The whole point of the type is that it is a unit-checked number, not a modular [0, 2pi) value.
        CHECK(tg::angle_f::make_from_degree(370) > tg::angle_f::make_from_degree(10));
        CHECK(tg::angle_f::make_from_degree(370) != tg::angle_f::make_from_degree(10));
    }

    SECTION("clamping, which is what an ordering is for")
    {
        auto const limit = tg::angle_f::make_from_degree(45);
        auto const clamped = b > limit ? limit : b;
        CHECK(tgtest::approx(clamped.degree(), 45.0f));
    }

    SECTION("a float angle is partially ordered, like the float it holds")
    {
        static_assert(std::is_same_v<decltype(a <=> b), std::partial_ordering>);
        CHECK((a <=> b) == std::partial_ordering::less);
    }
}

TEST("tg angle - trigonometry")
{
    auto const a = tg::angle_f::make_from_degree(45);
    auto const root_half = 0.70710678f; // sqrt(2)/2
    auto const root_two = 1.41421356f;  // sqrt(2)

    SECTION("member forms")
    {
        CHECK(tgtest::approx(a.sin(), root_half));
        CHECK(tgtest::approx(a.cos(), root_half));
        CHECK(tgtest::approx(a.tan(), 1.0f));
        CHECK(tgtest::approx(a.sec(), root_two));
        CHECK(tgtest::approx(a.csc(), root_two));
        CHECK(tgtest::approx(a.cot(), 1.0f));

        auto const sc = a.sin_cos();
        CHECK(tgtest::approx(sc.first, a.sin()));
        CHECK(tgtest::approx(sc.second, a.cos()));
    }

    SECTION("free forms agree with members")
    {
        CHECK(tg::sin(a) == a.sin());
        CHECK(tg::cos(a) == a.cos());
        CHECK(tg::tan(a) == a.tan());
        CHECK(tg::sec(a) == a.sec());
        CHECK(tg::csc(a) == a.csc());
        CHECK(tg::cot(a) == a.cot());

        auto const sc = tg::sin_cos(a);
        CHECK(sc.first == a.sin());
        CHECK(sc.second == a.cos());
    }
}

TEST("tg angle - literals")
{
    using namespace tg::literals;

    CHECK(tgtest::approx((90_deg_f).degree(), 90.0f));
    CHECK(tgtest::approx((180_deg_d).degree(), 180.0));
    CHECK(tgtest::approx((1.5_rad_f).radians(), 1.5f));
    CHECK(tgtest::approx((3.0_rad_d).radians(), 3.0));

    // literals are also visible unqualified inside tg (re-exported), but here we exercise the
    // public opt-in path
    CHECK(90_deg_f == tg::angle_f::make_from_degree(90));
}
