#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::quat_f>, "quat should be trivially copyable");

TEST("tg quat - identity")
{
    SECTION("default is zero, identity is (0,0,0,1)")
    {
        CHECK(tg::quat_f::zero == tg::quat_f(0, 0, 0, 0));
        CHECK(tg::quat_f::identity == tg::quat_f(0, 0, 0, 1));
    }

    SECTION("identity rotates a vector to itself")
    {
        auto const v = tg::vec3f(1, 2, 3);
        CHECK(tgtest::approx(tg::quat_f::identity * v, v));
    }
}

TEST("tg quat - rotations")
{
    auto const quarter = tg::angle_f::make_from_degree(90);
    auto const ex = tg::vec3f(1, 0, 0);
    auto const ey = tg::vec3f(0, 1, 0);
    auto const ez = tg::vec3f(0, 0, 1);

    SECTION("rotation_z maps x -> y")
    {
        auto const q = tg::quat_f::make_rotation_z(quarter);
        CHECK(tgtest::approx(q * ex, ey));
    }

    SECTION("composition: two 45s equal one 90")
    {
        auto const h = tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(45));
        auto const q = h * h;
        CHECK(tgtest::approx(q * ex, ey));
    }

    SECTION("agrees with the matrix rotation")
    {
        auto const q = tg::quat_f::make_rotation_axis_angle(ez, quarter);
        auto const m = tg::mat3f::make_rotation_z(quarter);
        CHECK(tgtest::approx(q * ex, m * ex));
        CHECK(tgtest::approx(q * ey, m * ey));
        CHECK(tgtest::approx(q * ez, m * ez));
    }
}

TEST("tg quat - axis and angle")
{
    auto const axis = tg::vec3f(0, 0, 1);
    auto const a = tg::angle_f::make_from_degree(50);
    auto const q = tg::quat_f::make_rotation_axis_angle(axis, a);

    SECTION("round-trips make_rotation_axis_angle")
    {
        CHECK(tgtest::approx(q.angle().degree(), 50.0f));
        CHECK(tgtest::approx(q.axis(), axis));
    }

    SECTION("identity has no rotation")
    {
        CHECK(tgtest::approx(tg::quat_f::identity.angle().degree(), 0.0f));
        CHECK(tg::quat_f::identity.axis() == tg::vec3f::zero);
    }
}

TEST("tg quat - measures")
{
    auto const q = tg::quat_f::make_rotation_x(tg::angle_f::make_from_degree(123));

    SECTION("unit quaternion has length 1")
    {
        CHECK(tgtest::approx(q.length(), 1.0f));
    }

    SECTION("conjugate undoes the rotation")
    {
        auto const v = tg::vec3f(1, 2, 3);
        CHECK(tgtest::approx(q.conjugate() * (q * v), v));
    }
}
