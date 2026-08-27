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
static_assert(tg::traits::has_pow2<float>, "f32 has the exact base-two operations");
static_assert(!tg::traits::has_pow2<int>, "shifting an integer truncates, so it is a different operation");

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

TEST("tg scalar - pow2 / scale_by_pow2")
{
    // exact, not approximate: this is an exponent adjustment, so equality is the right assertion
    CHECK(tg::pow2<float>(0) == 1.0f);
    CHECK(tg::pow2<float>(10) == 1024.0f);
    CHECK(tg::pow2<double>(-3) == 0.125);

    CHECK(tg::scale_by_pow2(3.0f, 4) == 48.0f);
    CHECK(tg::scale_by_pow2(3.0, -2) == 0.75);
    CHECK(tg::scale_by_pow2(-1.5f, 1) == -3.0f);
    CHECK(tg::scale_by_pow2(0.0f, 7) == 0.0f); // zero has no exponent to shift

    // a mantissa the scalar holds exactly survives any in-range shift, which a pow() round-trip would not promise
    auto const odd = 1.0f + 1.0f / 8388608.0f; // 1 + 2^-23, the last representable step above one
    CHECK(tg::scale_by_pow2(odd, 60) == odd * tg::pow2<float>(60));

    // out of range saturates rather than wrapping
    CHECK(tg::scale_by_pow2(1.0f, 400) > 1e38f);
    CHECK(tg::scale_by_pow2(1.0f, -400) == 0.0f);
}

TEST("tg scalar - exponent_of / split_pow2")
{
    // the significand is in [1, 2), so the exponent is floor(log2(|x|)) — NOT frexp's [0.5, 1) convention
    auto const eight = tg::split_pow2(8.0f);
    CHECK(eight.significand == 1.0f);
    CHECK(eight.exponent == 3);

    auto const twelve = tg::split_pow2(12.0);
    CHECK(twelve.significand == 1.5);
    CHECK(twelve.exponent == 3);

    CHECK(tg::exponent_of(1.0f) == 0);
    CHECK(tg::exponent_of(0.5f) == -1);
    CHECK(tg::exponent_of(1023.0f) == 9); // floor, so just under 1024 is still 9
    CHECK(tg::exponent_of(1024.0f) == 10);

    // the sign rides on the significand, leaving the exponent a pure magnitude
    auto const negative = tg::split_pow2(-5.0);
    CHECK(negative.significand == -1.25);
    CHECK(negative.exponent == 2);

    // the split is lossless: reassembling returns the original value
    for (auto const value : {1.0f, 0.1f, 1e20f, -3.75f, 1e-20f})
    {
        auto const split = tg::split_pow2(value);
        CHECK(tg::scale_by_pow2(split.significand, split.exponent) == value);
    }

    // subnormals are normalized rather than reported with a zero exponent, which a raw exponent-field read would do
    auto const subnormal = 1e-42f;
    auto const split = tg::split_pow2(subnormal);
    CHECK(split.significand >= 1.0f);
    CHECK(split.significand < 2.0f);
    CHECK(tg::scale_by_pow2(split.significand, split.exponent) == subnormal);
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
