#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/box.hh>
#include <typed-geometry/geometry/query/query.hh>
#include <typed-geometry/geometry/traits.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::box3f>, "box should be trivially copyable");
static_assert(std::is_trivially_copyable_v<tg::aabb3f_surface>, "aabb_boundary should be trivially copyable");

namespace
{
static_assert(tg::traits::intrinsic_dim<tg::box3f> == 3);
static_assert(tg::traits::intrinsic_dim<tg::box3f_surface> == 2);
static_assert(tg::traits::intrinsic_dim<tg::box2in3f> == 2, "a rectangle in 3D");
static_assert(tg::traits::ambient_dim<tg::box2in3f> == 3);
static_assert(tg::traits::intrinsic_dim<tg::aabb3f_surface> == 2);
static_assert(!std::is_convertible_v<tg::aabb3f, tg::aabb3f_surface>);
} // namespace

TEST("tg box - construction and readings")
{
    auto const h = tg::mat3f::make_from_cols(tg::vec3f(2, 0, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, 1));
    auto const b = tg::box3f(tg::pos3f(1, 2, 3), h);

    CHECK(b.center == tg::pos3f(1, 2, 3));
    CHECK(b.half_extents.cols[0] == tg::vec3f(2, 0, 0));
    CHECK(b.boundary().solid() == b);
}

TEST("tg box - an aabb becomes a box under a rotation")
{
    auto const a = tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(4, 2, 2));
    auto const t = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(90)));

    auto const b = a.transformed(t);
    static_assert(std::is_same_v<decltype(b), tg::box3f const>);

    // the center (2, 1, 1) rotates to (-1, 2, 1); the x half-axis of length 2 now points along +y
    CHECK(tgtest::approx(b.center, tg::pos3f(-1, 2, 1)));
    CHECK(tgtest::approx(b.half_extents.cols[0], tg::vec3f(0, 2, 0)));
    CHECK(tgtest::approx(b.half_extents.cols[1], tg::vec3f(-1, 0, 0)));

    // a scaling still keeps it an aabb, and its boundary follows
    auto const s = a.boundary().transformed(tg::scaling_transform3f::make_scaling(tg::vec3f(2, 2, 2)));
    static_assert(std::is_same_v<decltype(s), tg::aabb3f_surface const>);
    CHECK(s.max == tg::pos3f(8, 4, 4));
}

TEST("tg box - a shear keeps it a box")
{
    auto const h = tg::mat3f::make_from_cols(tg::vec3f(1, 0, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, 1));
    auto const b = tg::box3f(tg::pos3f(0, 0, 0), h);
    auto const shear = tg::affine_transform3f::make_from_linear_mat(
        tg::mat3f::make_from_cols(tg::vec3f(1, 0, 0), tg::vec3f(1, 1, 0), tg::vec3f(0, 0, 1)));

    auto const r = b.transformed(shear);
    CHECK(tgtest::approx(r.half_extents.cols[1], tg::vec3f(1, 1, 0)));
}

TEST("tg box - a point meets the surface exactly when it lies on it")
{
    static_assert(tg::has_intersects<tg::pos3f, tg::box3f_surface>);
    static_assert(tg::has_intersects<tg::box3f_surface, tg::pos3f>);

    auto const b = tg::box3f(tg::pos3f(0, 0, 0),
                             tg::mat3f::make_from_cols(tg::vec3f(1, 0, 0), tg::vec3f(0, 2, 0), tg::vec3f(0, 0, 4)));
    auto const s = b.boundary();
    auto const face_center = tg::pos3f(1, 0, 0);
    auto const corner = tg::pos3f(-1, 2, 4);
    CHECK(s.contains(face_center));
    CHECK(s.intersects(face_center));
    CHECK(face_center.intersects(s));
    CHECK(s.intersects(corner));
    CHECK(corner.intersects(s));
    CHECK(s.intersects(b.vertices()[0]));

    // inside the solid but off the surface, and outside both
    CHECK(!s.intersects(tg::pos3f(0.5f, 0, 0)));
    CHECK(!tg::pos3f(0.5f, 0, 0).intersects(s));
    CHECK(!s.intersects(tg::pos3f(2, 0, 0)));
    CHECK(b.intersects(tg::pos3f(0.5f, 0, 0)));
}
