#include <nexus/test.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::vec3f>, "vec should be trivially copyable");
static_assert(std::is_trivially_copyable_v<tg::vec2i>, "vec should be trivially copyable");

TEST("tg vec - construction")
{
    SECTION("default zero-inits")
    {
        tg::vec3f v;
        CHECK(v == tg::vec3f(0, 0, 0));
    }

    SECTION("splat")
    {
        auto const v = tg::vec4i(3);
        CHECK(v == tg::vec4i(3, 3, 3, 3));
    }

    SECTION("per-dimension")
    {
        CHECK(tg::vec2f(1, 2)[1] == 2);
        CHECK(tg::vec3f(1, 2, 3)[2] == 3);
        CHECK(tg::vec4f(1, 2, 3, 4)[3] == 4);
    }

    SECTION("initializer list and from_values agree")
    {
        auto const a = tg::vec3f({1, 2, 3});
        auto const b = tg::vec3f::from_values(1, 2, 3);
        CHECK(a == b);
    }

    SECTION("initializer list of wrong size asserts")
    {
        CHECK_ASSERTS(tg::vec3f({1, 2}));
    }
}

TEST("tg vec - arithmetic")
{
    auto const a = tg::vec3f(1, 2, 3);
    auto const b = tg::vec3f(4, 5, 6);

    SECTION("add / sub")
    {
        CHECK(a + b == tg::vec3f(5, 7, 9));
        CHECK(b - a == tg::vec3f(3, 3, 3));
    }

    SECTION("unary minus")
    {
        CHECK(-a == tg::vec3f(-1, -2, -3));
    }

    SECTION("scalar mul / div, both sides")
    {
        CHECK(a * 2.0f == tg::vec3f(2, 4, 6));
        CHECK(2.0f * a == tg::vec3f(2, 4, 6));
        CHECK(tg::vec3f(2, 4, 6) / 2.0f == a);
    }

    SECTION("compound assignment")
    {
        auto c = a;
        c += b;
        CHECK(c == tg::vec3f(5, 7, 9));
        c -= b;
        CHECK(c == a);
        c *= 3.0f;
        CHECK(c == tg::vec3f(3, 6, 9));
        c /= 3.0f;
        CHECK(c == a);
    }
}

TEST("tg vec - measures")
{
    SECTION("length_sqr for any scalar")
    {
        CHECK(tg::vec3i(1, 2, 2).length_sqr() == 9);
    }

    SECTION("length and normalized for floats")
    {
        auto const v = tg::vec3f(0, 3, 4);
        CHECK(v.length_sqr() == 25);
        CHECK(v.length() == 5);

        auto const n = v.normalized();
        auto const err = n.length() - 1.0f;
        CHECK(-1e-6f < err);
        CHECK(err < 1e-6f);
    }

    SECTION("normalizing zero asserts")
    {
        CHECK_ASSERTS(tg::vec3f(0, 0, 0).normalized());
    }

    SECTION("dot product")
    {
        CHECK(dot(tg::vec3f(1, 2, 3), tg::vec3f(4, 5, 6)) == 32);
        CHECK(dot(tg::vec3f(1, 0, 0), tg::vec3f(0, 1, 0)) == 0);
    }

    SECTION("free normalize matches member")
    {
        auto const v = tg::vec3f(0, 3, 4);
        CHECK(normalize(v) == v.normalized());
    }
}
