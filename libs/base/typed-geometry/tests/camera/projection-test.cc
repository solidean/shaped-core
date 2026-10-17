#include "../approx.hh"
#include "conventions.hh"

#include <nexus/test.hh>
#include <typed-geometry/camera/camera.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>

#include <type_traits>

namespace
{
auto const k_fov
    = tg::angle_d::make_from_degree(90); // tan(45 deg) = 1, so the near plane's half height is the near distance
auto constexpr k_aspect = 2.0;
auto constexpr k_near = 1.0;
auto constexpr k_far = 10.0;
} // namespace

TEST("tg camera - a perspective projection maps its view volume onto the clip volume, in every convention")
{
    for (auto const& fam : tgtest::k_projection_families)
        SECTION("{}", fam.name)
        {
            auto const p = fam.perspective(k_fov, k_aspect, k_near, k_far);

            CHECK(tgtest::approx(p(fam.ahead(0, 0, k_near)), tg::pos3d(0, 0, fam.near_depth())));
            CHECK(tgtest::approx(p(fam.ahead(0, 0, k_far)), tg::pos3d(0, 0, fam.far_depth())));
            // the near plane is 2 * k_near * k_aspect wide and 2 * k_near high
            CHECK(tgtest::approx(p(fam.ahead(2, 1, k_near)), tg::pos3d(1, 1, fam.near_depth())));
            CHECK(tgtest::approx(p(fam.ahead(-20, -10, k_far)), tg::pos3d(-1, -1, fam.far_depth())));
        }
}

TEST("tg camera - without a far distance the far plane is at infinity")
{
    for (auto const& fam : tgtest::k_projection_families)
        SECTION("{}", fam.name)
        {
            auto const p = fam.perspective_infinite(k_fov, k_aspect, k_near);

            CHECK(tgtest::approx(p(fam.ahead(0, 0, k_near)), tg::pos3d(0, 0, fam.near_depth())));
            CHECK(tgtest::approx(p(fam.ahead(2, 1, k_near)), tg::pos3d(1, 1, fam.near_depth())));
            CHECK(tgtest::approx(p(fam.ahead(0, 0, 1e9)).data[2], fam.far_depth(), 1e-6));
        }
}

TEST("tg camera - an off-center perspective projection maps its near rectangle onto the clip square")
{
    for (auto const& fam : tgtest::k_projection_families)
        SECTION("{}", fam.name)
        {
            auto const p = fam.perspective_off_center(-1, 3, -2, 1, k_near, k_far);

            CHECK(tgtest::approx(p(fam.ahead(3, 1, k_near)), tg::pos3d(1, 1, fam.near_depth())));
            CHECK(tgtest::approx(p(fam.ahead(-1, -2, k_near)), tg::pos3d(-1, -1, fam.near_depth())));
            // the rectangle grows with distance: the same corner direction, ten times as far
            CHECK(tgtest::approx(p(fam.ahead(30, 10, k_far)), tg::pos3d(1, 1, fam.far_depth())));
        }
}

TEST("tg camera - an orthographic projection maps its box onto the clip volume")
{
    for (auto const& fam : tgtest::k_projection_families)
        SECTION("{}", fam.name)
        {
            auto const o = fam.orthographic(4, 2, k_near, k_far);
            CHECK(tgtest::approx(o(fam.ahead(2, 1, k_near)), tg::pos3d(1, 1, fam.near_depth())));
            CHECK(tgtest::approx(o(fam.ahead(-2, -1, k_far)), tg::pos3d(-1, -1, fam.far_depth())));

            auto const off = fam.orthographic_off_center(-1, 3, -2, 1, k_near, k_far);
            CHECK(tgtest::approx(off(fam.ahead(3, 1, k_near)), tg::pos3d(1, 1, fam.near_depth())));
            CHECK(tgtest::approx(off(fam.ahead(-1, -2, k_far)), tg::pos3d(-1, -1, fam.far_depth())));
        }
}

TEST("tg camera - an orthographic projection of a view-space box, in every convention")
{
    for (auto const& fam : tgtest::k_projection_families)
        SECTION("{}", fam.name)
        {
            // the box's near face is k_near along the view axis, whichever way that points
            auto const a = fam.ahead(-1, -2, k_near);
            auto const b = fam.ahead(3, 1, k_far);
            auto const box = tg::aabb3d(tg::pos3d(-1, -2, fam.right_handed ? -k_far : k_near),
                                        tg::pos3d(3, 1, fam.right_handed ? -k_near : k_far));
            auto const o = fam.orthographic_box(box);
            CHECK(o == fam.orthographic_off_center(-1, 3, -2, 1, k_near, k_far));
            CHECK(tgtest::approx(o(a), tg::pos3d(-1, -1, fam.near_depth())));
            CHECK(tgtest::approx(o(b), tg::pos3d(1, 1, fam.far_depth())));
        }
}

TEST("tg camera - an orthographic projection's type says whether it flips depth")
{
    // a convention keeps the depth axis's direction when its near depth is below its far depth along the view axis
    static_assert(
        std::is_same_v<decltype(tg::orthographic_lh_zero_to_one(4.0, 2.0, 1.0, 10.0)), tg::scaling_translation_transform3d>);
    static_assert(std::is_same_v<decltype(tg::orthographic_lh_minus_one_to_one(4.0, 2.0, 1.0, 10.0)),
                                 tg::scaling_translation_transform3d>);
    static_assert(
        std::is_same_v<decltype(tg::orthographic_rh_reverse_z(4.0, 2.0, 1.0, 10.0)), tg::scaling_translation_transform3d>);
    static_assert(std::is_same_v<decltype(tg::orthographic_lh_reverse_z(4.0, 2.0, 1.0, 10.0)),
                                 tg::signed_scaling_translation_transform3d>);
    static_assert(std::is_same_v<decltype(tg::orthographic_rh_zero_to_one(4.0, 2.0, 1.0, 10.0)),
                                 tg::signed_scaling_translation_transform3d>);

    // so an aabb stays an aabb under one
    auto const box = tg::aabb3d(tg::pos3d(-1, -1, 2), tg::pos3d(1, 1, 3));
    auto const clip_box = box.transformed(tg::orthographic_lh_zero_to_one(4.0, 2.0, 1.0, 10.0));
    static_assert(std::is_same_v<decltype(clip_box), tg::aabb3d const>);
    CHECK(tgtest::approx(clip_box.max, tg::pos3d(0.5, 1, 2.0 / 9.0)));
}

TEST("tg camera - the unsuffixed functions are the left-handed reverse-Z ones")
{
    CHECK(tg::perspective(k_fov, k_aspect, k_near, k_far) == tg::perspective_lh_reverse_z(k_fov, k_aspect, k_near, k_far));
    CHECK(tg::perspective(k_fov, k_aspect, k_near) == tg::perspective_lh_reverse_z(k_fov, k_aspect, k_near));
    CHECK(tg::orthographic(4.0, 2.0, k_near, k_far) == tg::orthographic_lh_reverse_z(4.0, 2.0, k_near, k_far));
}

TEST("tg camera - an orthographic projection of a view-space box maps the box onto the clip volume")
{
    auto const o = tg::orthographic(tg::aabb3d(tg::pos3d(-1, -2, k_near), tg::pos3d(3, 1, k_far)));
    // reverse-Z: the box's near face goes to depth 1
    CHECK(tgtest::approx(o(tg::pos3d(-1, -2, k_near)), tg::pos3d(-1, -1, 1)));
    CHECK(tgtest::approx(o(tg::pos3d(3, 1, k_far)), tg::pos3d(1, 1, 0)));
}
