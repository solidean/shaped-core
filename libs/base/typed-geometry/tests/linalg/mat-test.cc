#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::mat3f>, "mat should be trivially copyable");

TEST("tg mat - identity and access")
{
    SECTION("default is zero, not identity")
    {
        tg::mat3f m;
        CHECK(m == tg::mat3f::zero);
        CHECK((m[0, 0]) == 0);
    }

    SECTION("identity has ones on the diagonal")
    {
        auto const id = tg::mat3f::identity;
        CHECK((id[0, 0]) == 1);
        CHECK((id[1, 1]) == 1);
        CHECK((id[2, 2]) == 1);
        CHECK((id[1, 0]) == 0);
        CHECK((id[0, 1]) == 0);
    }

    SECTION("col() returns column vectors")
    {
        auto const id = tg::mat3f::identity;
        CHECK(id.col(0) == tg::vec3f(1, 0, 0));
        CHECK(id.col(2) == tg::vec3f(0, 0, 1));
    }

    SECTION("out-of-range asserts")
    {
        tg::mat3f m;
        CHECK_ASSERTS((m[3, 0]));
        CHECK_ASSERTS((m[0, 3]));
    }
}

TEST("tg mat - products")
{
    auto const id = tg::mat3f::identity;
    auto const v = tg::vec3f(2, 3, 4);

    SECTION("identity is neutral")
    {
        CHECK(id * v == v);
        CHECK(id * id == id);
    }

    SECTION("make_from_cols")
    {
        auto const m = tg::mat3f::make_from_cols(tg::vec3f(1, 0, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, 1));
        CHECK(m == id);
    }
}

TEST("tg mat - rotations")
{
    auto const quarter = tg::angle_f::make_from_degree(90);
    auto const ex = tg::vec3f(1, 0, 0);
    auto const ey = tg::vec3f(0, 1, 0);
    auto const ez = tg::vec3f(0, 0, 1);

    SECTION("rotation_z maps x -> y")
    {
        auto const r = tg::mat3f::make_rotation_z(quarter);
        CHECK(tgtest::approx(r * ex, ey));
    }

    SECTION("rotation_x maps y -> z")
    {
        auto const r = tg::mat3f::make_rotation_x(quarter);
        CHECK(tgtest::approx(r * ey, ez));
    }

    SECTION("rotation_y maps z -> x")
    {
        auto const r = tg::mat3f::make_rotation_y(quarter);
        CHECK(tgtest::approx(r * ez, ex));
    }

    SECTION("axis-angle about z equals rotation_z")
    {
        auto const r1 = tg::mat3f::make_rotation_axis_angle(ez, quarter);
        auto const r2 = tg::mat3f::make_rotation_z(quarter);
        CHECK(tgtest::approx(r1 * ex, r2 * ex));
        CHECK(tgtest::approx(r1 * ey, r2 * ey));
    }
}
