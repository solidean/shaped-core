#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/linalg/dual_quat.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/angle.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::dual_quat_f>, "dual_quat should be trivially copyable");

namespace
{
[[nodiscard]] tg::dual_quat_f turn_z(float degrees, tg::vec3f translation = tg::vec3f::zero)
{
    return tg::dual_quat_f::make_rigid(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(degrees)), translation);
}

/// a rotation about the z axis through `pivot`, which it leaves where it is
[[nodiscard]] tg::dual_quat_f turn_about(tg::pos3f pivot, float degrees)
{
    auto const to = tg::vec3f(pivot.data[0], pivot.data[1], pivot.data[2]);
    return tg::dual_quat_f::make_translation(to) * turn_z(degrees) * tg::dual_quat_f::make_translation(-1.0f * to);
}
} // namespace

TEST("tg dual_quat - a rigid motion rotates first, then translates")
{
    auto const d = turn_z(90, tg::vec3f(1, 0, 0));
    CHECK(tgtest::approx(d * tg::pos3f(1, 0, 0), tg::pos3f(1, 1, 0)));
    CHECK(tgtest::approx(d * tg::vec3f(1, 0, 0), tg::vec3f(0, 1, 0)));
    CHECK(tgtest::approx(d.translation(), tg::vec3f(1, 0, 0)));
    CHECK(tgtest::approx(tg::dual_quat_f::identity * tg::pos3f(1, 2, 3), tg::pos3f(1, 2, 3)));
    CHECK(tg::dual_quat_f::zero == tg::dual_quat_f(tg::quat_f::zero, tg::quat_f::zero));
}

TEST("tg dual_quat - a product applies its right factor first, and the inverse undoes")
{
    auto const a = turn_z(90, tg::vec3f(1, 0, 0));
    auto const b = turn_z(30, tg::vec3f(0, 2, 1));
    auto const p = tg::pos3f(0.5f, -1, 2);
    CHECK(tgtest::approx((a * b) * p, a * (b * p)));
    CHECK(tgtest::approx(a.inverse() * (a * p), p));
}

TEST("tg dual_quat - normalized gives a rigid motion back, its dual part orthogonal to its real part")
{
    auto const d = turn_z(40, tg::vec3f(1, 2, 3));
    // scaled, and pushed off the unit dual quaternions along the real part
    auto const off = tg::dual_quat_f(d.real * 2.0f, d.dual * 2.0f + d.real * 0.3f);
    auto const n = off.normalized();
    CHECK(tgtest::approx(n.real.length(), 1.0f));
    CHECK(tgtest::approx(tg::dot(n.real, n.dual), 0.0f));
    CHECK(tgtest::approx(n.translation(), tg::vec3f(1, 2, 3)));
    CHECK(tg::dual_quat_f::zero.normalized() == tg::dual_quat_f::zero);
}

TEST("tg dual_quat - a blend keeps a pivot both motions share, and d and -d blend as one motion")
{
    auto const pivot = tg::pos3f(1, 2, 0);
    tg::dual_quat_f const motions[] = {turn_about(pivot, 0), turn_about(pivot, 90)};
    float const weights[] = {0.5f, 0.5f};
    auto const half = tg::blend<float>(motions, weights);
    CHECK(tgtest::approx(half * pivot, pivot));
    // halfway: a quarter turn's half, an eighth, and no shrinking towards the pivot as a blend of points would
    auto const p = pivot + tg::vec3f(1, 0, 0);
    auto const q = half * p;
    CHECK(tgtest::approx((q - pivot).length(), 1.0f));
    CHECK(tgtest::approx(q, pivot + tg::vec3f(0.70710678f, 0.70710678f, 0)));

    auto const d = turn_z(70, tg::vec3f(1, 0, 0));
    tg::dual_quat_f const sides[] = {d, -d};
    auto const same = tg::blend<float>(sides, weights);
    CHECK(tgtest::approx(same * p, d * p));
}

TEST("tg dual_quat - sclerp follows one screw from a to b")
{
    // a quarter turn about the vertical axis through (1, 0, 0), sliding 2 along it
    auto const axis_point = tg::pos3f(1, 0, 0);
    auto const a = tg::dual_quat_f::identity;
    auto const b = tg::dual_quat_f::make_translation(tg::vec3f(0, 0, 2)) * turn_about(axis_point, 90);
    auto const p = tg::pos3f(2, 0, 0);
    CHECK(tgtest::approx(tg::sclerp(a, b, 0.0f) * p, a * p));
    CHECK(tgtest::approx(tg::sclerp(a, b, 1.0f) * p, b * p));

    // halfway: an eighth turn, half the slide, and the axis stays where it was
    auto const mid = tg::sclerp(a, b, 0.5f);
    CHECK(tgtest::approx(mid * axis_point, axis_point + tg::vec3f(0, 0, 1)));
    CHECK(tgtest::approx(mid * p, tg::pos3f(1.70710678f, 0.70710678f, 1)));

    // no turn at all: a straight slide
    auto const slide = tg::dual_quat_f::make_translation(tg::vec3f(4, 0, 0));
    CHECK(tgtest::approx(tg::sclerp(a, slide, 0.25f) * p, tg::pos3f(3, 0, 0)));
}
