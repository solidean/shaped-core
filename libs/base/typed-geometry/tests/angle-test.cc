#include "approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/constants.hh>

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
