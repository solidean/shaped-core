#include <nexus/test.hh>
#include <typed-geometry/linalg/bivec.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::bivec3f>, "bivec should be trivially copyable");
static_assert(tg::bivec2f::num_components == 1, "bivec2 has 1 component");
static_assert(tg::bivec3f::num_components == 3, "bivec3 has 3 components");
static_assert(tg::bivec4f::num_components == 6, "bivec4 has 6 components");

TEST("tg bivec - basics")
{
    SECTION("default zero / zero static")
    {
        tg::bivec3f b;
        CHECK(b == tg::bivec3f::zero);
        CHECK(b[0] == 0);
    }

    SECTION("construction and arithmetic")
    {
        auto const b = tg::bivec3f::make_from_values(1, 2, 3);
        CHECK(b[0] == 1);
        CHECK(b[2] == 3);
        CHECK(b + b == tg::bivec3f::make_from_values(2, 4, 6));
        CHECK(2.0f * b == tg::bivec3f::make_from_values(2, 4, 6));
        CHECK(-b == tg::bivec3f::make_from_values(-1, -2, -3));
    }

    SECTION("out-of-range asserts")
    {
        tg::bivec3f b;
        CHECK_ASSERTS(b[3]);
    }
}

TEST("tg bivec - cross / dual / undual")
{
    auto const ex = tg::vec3f(1, 0, 0);
    auto const ey = tg::vec3f(0, 1, 0);
    auto const ez = tg::vec3f(0, 0, 1);

    SECTION("dual(cross) reproduces the classic cross product")
    {
        // ex x ey = ez
        CHECK(dual(cross(ex, ey)) == ez);
        CHECK(dual(cross(ey, ez)) == ex);
        CHECK(dual(cross(ez, ex)) == ey);
    }

    SECTION("dual and undual are inverses")
    {
        auto const b = tg::cross(tg::vec3f(1, 2, 3), tg::vec3f(4, 5, 6));
        CHECK(undual(dual(b)) == b);

        auto const v = tg::vec3f(7, 8, 9);
        CHECK(dual(undual(v)) == v);
    }

    SECTION("anti-commutativity")
    {
        CHECK(cross(ex, ey) == -cross(ey, ex));
    }
}
