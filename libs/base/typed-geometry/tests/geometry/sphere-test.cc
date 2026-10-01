#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/sphere.hh>
#include <typed-geometry/geometry/traits.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::sphere3f>, "sphere should be trivially copyable");
static_assert(std::is_trivially_copyable_v<tg::sphere3f_surface>, "sphere_boundary should be trivially copyable");

namespace
{
// a sphere is the SOLID ball, so it is full-dimensional; its boundary is codimension 1
static_assert(tg::traits::intrinsic_dim<tg::sphere3f> == 3);
static_assert(tg::traits::ambient_dim<tg::sphere3f> == 3);
static_assert(tg::traits::is_finite<tg::sphere3f>);
static_assert(tg::traits::intrinsic_dim<tg::sphere2f> == 2, "a 2D sphere is a disk");
static_assert(tg::traits::intrinsic_dim<tg::sphere3f_surface> == 2);
static_assert(tg::traits::intrinsic_dim<tg::sphere2f_boundary> == 1, "the boundary of a disk is a circle");

// a disk keeps its own dimension when it is embedded in 3D — only the ambient one grows
static_assert(tg::traits::intrinsic_dim<tg::disk3f> == 2);
static_assert(tg::traits::ambient_dim<tg::disk3f> == 3);
static_assert(tg::traits::intrinsic_dim<tg::circle3f> == 1);
static_assert(std::is_same_v<tg::disk3f, tg::sphere2in3f>);
static_assert(std::is_same_v<tg::circle3f, tg::sphere2in3f_boundary>);
static_assert(std::is_same_v<tg::sphere3f_surface, tg::sphere3f_boundary>);

// the embedded case pays for the normal of the plane it lies in; the flat one stores nothing extra
static_assert(sizeof(tg::sphere3f) == sizeof(float) * 4);
static_assert(sizeof(tg::sphere2in3f) == sizeof(float) * 7);
static_assert(sizeof(tg::sphere3f_surface) == sizeof(tg::sphere3f));

// the two readings never convert implicitly
static_assert(!std::is_convertible_v<tg::sphere3f, tg::sphere3f_surface>);
static_assert(!std::is_convertible_v<tg::sphere3f_surface, tg::sphere3f>);
} // namespace

TEST("tg sphere - construction")
{
    SECTION("default is a degenerate point sphere at the origin")
    {
        auto const s = tg::sphere3f();
        CHECK(s.center == tg::pos3f::zero);
        CHECK(s.radius == 0.0f);
    }

    SECTION("explicit construction keeps center and radius")
    {
        auto const s = tg::sphere3f(tg::pos3f(1, 2, 3), 4.0f);
        CHECK(s.center == tg::pos3f(1, 2, 3));
        CHECK(s.radius == 4.0f);
    }

    SECTION("equality is member-wise")
    {
        CHECK(tg::sphere3f(tg::pos3f(1, 0, 0), 2.0f) == tg::sphere3f(tg::pos3f(1, 0, 0), 2.0f));
        CHECK(tg::sphere3f(tg::pos3f(1, 0, 0), 2.0f) != tg::sphere3f(tg::pos3f(1, 0, 0), 3.0f));
    }
}

TEST("tg sphere - the boundary is a type of its own")
{
    auto const s = tg::sphere3f(tg::pos3f(1, 2, 3), 4.0f);

    SECTION("boundary and solid round-trip the encoding")
    {
        auto const b = s.boundary();
        static_assert(std::is_same_v<decltype(b), tg::sphere3f_surface const>);
        CHECK(b.center == s.center);
        CHECK(b.radius == s.radius);
        CHECK(b.solid() == s);
    }

    SECTION("a boundary transforms as its solid does")
    {
        auto const t = tg::similarity_transform3f::make_uniform_scaling(2.0f);
        auto const r = s.boundary().transformed(t);
        static_assert(std::is_same_v<decltype(r), tg::sphere3f_surface const>);
        CHECK(r == s.transformed(t).boundary());
    }

    SECTION("an affine map turns the surface into an ellipsoid surface")
    {
        auto const r = s.boundary().transformed(tg::scaling_transform3f::make_scaling(tg::vec3f(1, 2, 3)));
        static_assert(std::is_same_v<decltype(r), tg::ellipsoid3f_surface const>);
        CHECK(tgtest::approx(r.semi_axes[2], tg::vec3f(0, 0, 12), 1e-4f));
    }

    SECTION("a circle in 3D is the boundary of a disk")
    {
        auto const d = tg::disk3f(tg::pos3f(0, 0, 0), 1.0f, tg::vec3f(0, 0, 1));
        auto const c = d.boundary();
        static_assert(std::is_same_v<decltype(c), tg::circle3f const>);
        CHECK(c.normal == d.normal);
        CHECK(c.solid() == d);
    }
}

TEST("tg sphere - a disk embedded in 3D carries its plane")
{
    auto const c = tg::disk3f(tg::pos3f(1, 2, 3), 2.0f, tg::vec3f(0, 0, 1));

    SECTION("construction keeps the normal alongside center and radius")
    {
        CHECK(c.center == tg::pos3f(1, 2, 3));
        CHECK(c.radius == 2.0f);
        CHECK(c.normal == tg::vec3f(0, 0, 1));

        // two disks that differ only in their plane are different objects
        CHECK(c != tg::sphere2in3f(tg::pos3f(1, 2, 3), 2.0f, tg::vec3f(0, 1, 0)));
    }

    SECTION("a rotation tilts the plane the disk lies in")
    {
        auto const t
            = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_x(tg::angle_f::make_from_degree(90)));
        auto const r = c.transformed(t);
        static_assert(std::is_same_v<decltype(r), tg::sphere2in3f const>);

        CHECK(tgtest::approx(r.radius, 2.0f));
        CHECK(tgtest::approx(r.normal, tg::vec3f(0, -1, 0), 1e-4f));
    }

    SECTION("a uniform scaling scales the radius and leaves the normal unit-length")
    {
        auto const r = c.transformed(tg::similarity_transform3f::make_uniform_scaling(3.0f));

        CHECK(tgtest::approx(r.radius, 6.0f));
        // the scale is divided out of the normal rather than carried into it
        CHECK(tgtest::approx(r.normal, tg::vec3f(0, 0, 1), 1e-4f));
    }
}

TEST("tg sphere - an affine map turns a disk in 3D into an ellipse in 3D")
{
    auto const d = tg::disk3f(tg::pos3f(0, 0, 0), 1.0f, tg::vec3f(0, 0, 1));
    auto const e = d.transformed(tg::scaling_transform3f::make_scaling(tg::vec3f(2, 3, 1)));
    static_assert(std::is_same_v<decltype(e), tg::ellipsoid2in3f const>);

    // whatever basis the disk's plane got, the image's semi-axes stay in the plane and span the scaled ellipse
    CHECK(tgtest::approx(e.semi_axes[0].data[2], 0.0f));
    CHECK(tgtest::approx(e.semi_axes[1].data[2], 0.0f));
    CHECK(tgtest::approx(e.area(), 6.0f * tg::pi<float>, 1e-3f));
}
