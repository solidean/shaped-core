#include <nexus/test.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/pos_ops.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::pos3f>, "pos should be trivially copyable");

TEST("tg pos - construction")
{
    SECTION("default is the origin")
    {
        tg::pos3f p;
        CHECK(p == tg::pos3f(0, 0, 0));
    }

    SECTION("per-dimension and from_values")
    {
        CHECK(tg::pos2i(1, 2) == tg::pos2i::from_values(1, 2));
        CHECK(tg::pos3f(1, 2, 3)[2] == 3);
    }
}

TEST("tg pos - affine arithmetic")
{
    auto const p = tg::pos3f(1, 2, 3);
    auto const q = tg::pos3f(4, 6, 3);

    SECTION("pos - pos -> vec")
    {
        tg::vec3f const d = q - p;
        CHECK(d == tg::vec3f(3, 4, 0));
    }

    SECTION("pos + vec -> pos and back")
    {
        auto const d = q - p;
        CHECK(p + d == q);
        CHECK(d + p == q); // vec + pos
        CHECK(q - d == p);
    }

    SECTION("pos + pos -> pos (translation of the singleton)")
    {
        CHECK(p + q == tg::pos3f(5, 8, 6));
    }

    SECTION("compound assignment with vec")
    {
        auto r = p;
        auto const d = q - p;
        r += d;
        CHECK(r == q);
        r -= d;
        CHECK(r == p);
    }
}

TEST("tg pos - distance")
{
    auto const p = tg::pos3f(0, 0, 0);
    auto const q = tg::pos3f(0, 3, 4);

    CHECK(distance_sqr(p, q) == 25);
    CHECK(distance(p, q) == 5);

    // distance_sqr is available for integer scalars (no sqrt needed)
    CHECK(distance_sqr(tg::pos2i(0, 0), tg::pos2i(3, 4)) == 25);
}
