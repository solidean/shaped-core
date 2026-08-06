#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/sphere.hh>
#include <typed-geometry/geometry/traits.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::sphere3f>, "sphere should be trivially copyable");

namespace
{
// a sphere is the SURFACE, so it is codimension 1 — a future tg::ball will reuse the encoding
static_assert(tg::traits::intrinsic_dim<tg::sphere3f> == 2);
static_assert(tg::traits::ambient_dim<tg::sphere3f> == 3);
static_assert(tg::traits::is_finite<tg::sphere3f>);
static_assert(tg::traits::intrinsic_dim<tg::sphere2f> == 1, "a 2D sphere is a circle");

// that circle keeps its own dimension when it is embedded in 3D — only the ambient one grows
static_assert(tg::traits::intrinsic_dim<tg::sphere2in3f> == 1);
static_assert(tg::traits::ambient_dim<tg::sphere2in3f> == 3);

// the embedded case pays for the normal of the plane it lies in; the flat one stores nothing extra
static_assert(sizeof(tg::sphere3f) == sizeof(float) * 4);
static_assert(sizeof(tg::sphere2in3f) == sizeof(float) * 7);
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

TEST("tg sphere - a circle embedded in 3D carries its plane")
{
    auto const c = tg::sphere2in3f(tg::pos3f(1, 2, 3), 2.0f, tg::vec3f(0, 0, 1));

    SECTION("construction keeps the normal alongside center and radius")
    {
        CHECK(c.center == tg::pos3f(1, 2, 3));
        CHECK(c.radius == 2.0f);
        CHECK(c.normal == tg::vec3f(0, 0, 1));

        // two circles that differ only in their plane are different objects
        CHECK(c != tg::sphere2in3f(tg::pos3f(1, 2, 3), 2.0f, tg::vec3f(0, 1, 0)));
    }

    SECTION("a rotation tilts the plane the circle lies in")
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
