#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <type_traits>

static_assert(tg::traits::has_sqrt<float>, "f32 has sqrt");
static_assert(!tg::traits::has_sqrt<int>, "i32 has no sqrt");
static_assert(tg::traits::has_trigonometry<double>, "f64 has trigonometry");
static_assert(!tg::traits::has_trigonometry<int>, "i32 has no trigonometry");
static_assert(tg::traits::has_exponential<float>, "f32 has exponentials");
static_assert(!tg::traits::has_exponential<int>, "i32 has no exponentials");
static_assert(tg::traits::has_rounding<double>, "f64 has rounding");
static_assert(!tg::traits::has_rounding<int>, "rounding an integer is the identity, so it is not offered");
static_assert(tg::traits::has_abs<int>, "integers have abs");
static_assert(!tg::traits::has_abs<bool>, "bool has no magnitude");

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

TEST("tg scalar - abs")
{
    CHECK(tg::abs(-2.5) == 2.5);
    CHECK(tg::abs(2.5) == 2.5);
    CHECK(tg::abs(-7) == 7);
    CHECK(tg::abs(7u) == 7u); // unsigned: the identity

    // -0.0 has a sign bit but no magnitude, and abs clears it
    CHECK(tg::abs(-0.0f) == 0.0f);

    static_assert(tg::abs(-3) == 3, "integer abs is constexpr");
}

TEST("tg scalar - pow / exp / log")
{
    CHECK(tgtest::approx(tg::pow(2.0f, 10.0f), 1024.0f));
    CHECK(tgtest::approx(tg::pow(9.0, 0.5), 3.0));

    // a negative exponent is the reciprocal — what an exponential zoom step relies on
    CHECK(tgtest::approx(tg::pow(2.0, -2.0), 0.25));

    CHECK(tgtest::approx(tg::exp(0.0), 1.0));
    CHECK(tgtest::approx(tg::log(1.0), 0.0));

    // log and exp invert each other
    CHECK(tgtest::approx(tg::log(tg::exp(2.5)), 2.5));
    CHECK(tgtest::approx(tg::exp(tg::log(2.5)), 2.5));
}

TEST("tg scalar - round / floor / ceil")
{
    CHECK(tg::round(2.4) == 2.0);
    CHECK(tg::round(2.6) == 3.0);
    CHECK(tg::floor(2.9) == 2.0);
    CHECK(tg::ceil(2.1) == 3.0);

    // halfway rounds away from zero, so the two signs stay symmetric — this is where std::round and a
    // nearest-even rule disagree
    CHECK(tg::round(2.5) == 3.0);
    CHECK(tg::round(-2.5) == -3.0);
    CHECK(tg::round(3.5) == 4.0);

    // negatives: floor goes down, ceil goes up, neither truncates toward zero
    CHECK(tg::floor(-2.1) == -3.0);
    CHECK(tg::ceil(-2.9) == -2.0);

    // the return type stays the scalar's, so narrowing is the caller's explicit step
    static_assert(std::is_same_v<decltype(tg::round(1.0f)), float>, "round keeps the scalar type");
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
