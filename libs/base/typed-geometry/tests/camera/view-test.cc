#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/camera/camera.hh>

#include <type_traits>

namespace
{
auto const k_eye = tg::pos3d(1, 2, -5);
auto const k_target = tg::pos3d(1, 2, 3);
auto const k_up = tg::vec3d(0, 1, 0);
} // namespace

TEST("tg camera - look_at puts the eye at the origin and the target ahead")
{
    static_assert(std::is_same_v<decltype(tg::look_at(k_eye, k_target)), tg::rigid_transform3d>);

    SECTION("left-handed: ahead is +z")
    {
        auto const v = tg::look_at(k_eye, k_target, k_up);
        CHECK(tgtest::approx(v(k_eye), tg::pos3d(0, 0, 0)));
        CHECK(tgtest::approx(v(k_target), tg::pos3d(0, 0, 8)));
    }
    SECTION("right-handed: ahead is -z")
    {
        auto const v = tg::look_at_rh(k_eye, k_target, k_up);
        CHECK(tgtest::approx(v(k_eye), tg::pos3d(0, 0, 0)));
        CHECK(tgtest::approx(v(k_target), tg::pos3d(0, 0, -8)));
    }
}

TEST("tg camera - up stays up, and the two handednesses see world +x on opposite sides")
{
    auto const lh = tg::look_at(k_eye, k_target, k_up);
    auto const rh = tg::look_at_rh(k_eye, k_target, k_up);

    // looking along world +z with +y up: left-handed, +x is to the right; right-handed, it is to the left
    CHECK(tgtest::approx(lh(k_eye + tg::vec3d(0, 1, 0)), tg::pos3d(0, 1, 0)));
    CHECK(tgtest::approx(rh(k_eye + tg::vec3d(0, 1, 0)), tg::pos3d(0, 1, 0)));
    CHECK(tgtest::approx(lh(k_eye + tg::vec3d(1, 0, 0)), tg::pos3d(1, 0, 0)));
    CHECK(tgtest::approx(rh(k_eye + tg::vec3d(1, 0, 0)), tg::pos3d(-1, 0, 0)));
}

TEST("tg camera - up only fixes the roll")
{
    // a tilted, unnormalized up gives the same view as a perpendicular unit one
    auto const tilted = tg::look_at(k_eye, k_target, tg::vec3d(0, 3, 2));
    auto const plain = tg::look_at(k_eye, k_target, k_up);
    auto const p = tg::pos3d(4, -1, 7);
    CHECK(tgtest::approx(tilted(p), plain(p)));
}

TEST("tg camera - look_to is look_at along a direction")
{
    auto const p = tg::pos3d(-3, 0.5, 2);
    CHECK(tgtest::approx(tg::look_to(k_eye, k_target - k_eye, k_up)(p), tg::look_at(k_eye, k_target, k_up)(p)));
    CHECK(tgtest::approx(tg::look_to_rh(k_eye, k_target - k_eye, k_up)(p), tg::look_at_rh(k_eye, k_target, k_up)(p)));
}
