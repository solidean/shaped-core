#include "approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/scalar/scalar.hh>

static_assert(tg::traits::has_sqrt<float>, "f32 has sqrt");
static_assert(!tg::traits::has_sqrt<int>, "i32 has no sqrt");
static_assert(tg::traits::has_trigonometry<double>, "f64 has trigonometry");
static_assert(!tg::traits::has_trigonometry<int>, "i32 has no trigonometry");

TEST("tg scalar - one / is_zero / is_one")
{
    SECTION("one across scalar kinds")
    {
        CHECK(tg::one<float>() == 1.0f);
        CHECK(tg::one<int>() == 1);
        CHECK(tg::one<unsigned char>() == 1); // char-likes that are integers
        CHECK(tg::one<signed char>() == 1);
        CHECK(tg::one<bool>() == true); // bool has its own specialization
    }

    SECTION("is_zero / is_one")
    {
        CHECK(tg::traits::is_zero(0.0f));
        CHECK(!tg::traits::is_zero(0.5f));
        CHECK(tg::traits::is_one(1));
        CHECK(!tg::traits::is_one(2));
        CHECK(tg::traits::is_zero(false));
        CHECK(tg::traits::is_one(true));
    }
}

TEST("tg scalar - sqrt / trig / atan2")
{
    CHECK(tg::sqrt(16.0f) == 4.0f);

    // trig is angle-typed: sin/cos take an angle, atan2 returns one
    auto const zero = tg::angle_f::make_from_radians(0.0f);
    CHECK(tgtest::approx(tg::sin(zero), 0.0f));
    CHECK(tgtest::approx(tg::cos(zero), 1.0f));

    auto const sc = tg::sin_cos(tg::angle_d::make_from_radians(0.0));
    CHECK(tgtest::approx(sc.first, 0.0));
    CHECK(tgtest::approx(sc.second, 1.0));

    auto const a = tg::atan2(1.0f, 1.0f); // -> angle_f
    CHECK(tgtest::approx(a.radians(), tg::pi<float> / 4));
    CHECK(tgtest::approx(a.degree(), 45.0f));
}

TEST("tg scalar - inverse trig returns angles")
{
    // asin/acos/atan take a scalar and return an angle
    CHECK(tgtest::approx(tg::asin(1.0f).degree(), 90.0f));
    CHECK(tgtest::approx(tg::acos(0.0f).degree(), 90.0f));
    CHECK(tgtest::approx(tg::atan(1.0f).degree(), 45.0f));

    // round-trips with the forward members
    auto const a = tg::angle_f::make_from_degree(30);
    CHECK(tgtest::approx(tg::asin(a.sin()).degree(), 30.0f));
    CHECK(tgtest::approx(tg::acos(a.cos()).degree(), 30.0f));
    CHECK(tgtest::approx(tg::atan(a.tan()).degree(), 30.0f));
}
