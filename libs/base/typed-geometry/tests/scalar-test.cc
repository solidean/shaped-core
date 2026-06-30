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
    CHECK(tgtest::approx(tg::sin(0.0f), 0.0f));
    CHECK(tgtest::approx(tg::cos(0.0f), 1.0f));

    auto const sc = tg::sin_cos(0.0);
    CHECK(tgtest::approx(sc.first, 0.0));
    CHECK(tgtest::approx(sc.second, 1.0));

    CHECK(tgtest::approx(tg::atan2(1.0f, 1.0f), tg::pi<float> / 4));
}
