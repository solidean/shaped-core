#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/ellipsoid.hh>
#include <typed-geometry/geometry/traits.hh>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<tg::ellipsoid3f>, "ellipsoid should be trivially copyable");

namespace
{
static_assert(tg::traits::intrinsic_dim<tg::ellipsoid3f> == 2);
static_assert(tg::traits::ambient_dim<tg::ellipsoid3f> == 3);
static_assert(tg::traits::is_finite<tg::ellipsoid3f>);

// an ellipse is the 2D case, and it keeps its own dimension when it is embedded in 3D
static_assert(tg::traits::intrinsic_dim<tg::ellipsoid2f> == 1);
static_assert(tg::traits::ambient_dim<tg::ellipsoid2f> == 2);
static_assert(tg::traits::intrinsic_dim<tg::ellipsoid2in3f> == 1);
static_assert(tg::traits::ambient_dim<tg::ellipsoid2in3f> == 3);

static_assert(std::is_same_v<tg::ellipsoid3<float>, tg::ellipsoid3f>);
static_assert(std::is_same_v<tg::ellipsoid2in3<float>, tg::ellipsoid2in3f>);

// the embedded case stores its semi-axes in ambient coordinates, so it is wider than the flat one
static_assert(sizeof(tg::ellipsoid2f) == sizeof(float) * (2 + 2 * 2));
static_assert(sizeof(tg::ellipsoid2in3f) == sizeof(float) * (3 + 2 * 3));
} // namespace

TEST("tg ellipsoid - construction")
{
    SECTION("the semi-axes are stored in order")
    {
        auto const e = tg::ellipsoid3f(tg::pos3f(1, 1, 1), tg::vec3f(2, 0, 0), tg::vec3f(0, 3, 0), tg::vec3f(0, 0, 4));

        CHECK(e.center == tg::pos3f(1, 1, 1));
        CHECK(e.semi_axes[0] == tg::vec3f(2, 0, 0));
        CHECK(e.semi_axes[2] == tg::vec3f(0, 0, 4));
    }

    SECTION("equality is member-wise")
    {
        auto const x = tg::vec3f(1, 0, 0);
        auto const y = tg::vec3f(0, 1, 0);
        auto const z = tg::vec3f(0, 0, 1);

        CHECK(tg::ellipsoid3f(tg::pos3f(0, 0, 0), x, y, z) == tg::ellipsoid3f(tg::pos3f(0, 0, 0), x, y, z));
        CHECK(tg::ellipsoid3f(tg::pos3f(0, 0, 0), x, y, z) != tg::ellipsoid3f(tg::pos3f(1, 0, 0), x, y, z));
        CHECK(tg::ellipsoid3f(tg::pos3f(0, 0, 0), x, y, z) != tg::ellipsoid3f(tg::pos3f(0, 0, 0), x * 2.0f, y, z));
    }
}

TEST("tg ellipsoid - transformation maps every semi-axis")
{
    auto const e = tg::ellipsoid3f(tg::pos3f(0, 0, 0), tg::vec3f(2, 0, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, 1));

    SECTION("a rotation rotates the semi-axes")
    {
        auto const t
            = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(90)));
        auto const r = e.transformed(t);

        CHECK(tgtest::approx(r.semi_axes[0], tg::vec3f(0, 2, 0), 1e-4f));
        CHECK(tgtest::approx(r.semi_axes[1], tg::vec3f(-1, 0, 0), 1e-4f));
    }

    SECTION("a scaling scales them")
    {
        auto const r = e.transformed(tg::scaling_transform3f::make_scaling(tg::vec3f(1, 3, 1)));
        CHECK(tgtest::approx(r.semi_axes[0], tg::vec3f(2, 0, 0), 1e-4f));
        CHECK(tgtest::approx(r.semi_axes[1], tg::vec3f(0, 3, 0), 1e-4f));
    }

    SECTION("a translation leaves them alone")
    {
        auto const r = e.transformed(tg::translation_transform3f::make_translation(tg::vec3f(1, 2, 3)));

        CHECK(tgtest::approx(r.center, tg::pos3f(1, 2, 3), 1e-4f));
        CHECK(tgtest::approx(r.semi_axes[0], tg::vec3f(2, 0, 0), 1e-4f));
    }
}

TEST("tg ellipsoid - an ellipse in 2D")
{
    auto const e = tg::ellipsoid2f(tg::pos2f(1, 0), tg::vec2f(2, 0), tg::vec2f(0, 1));

    CHECK(e.center == tg::pos2f(1, 0));
    CHECK(e.semi_axes[1] == tg::vec2f(0, 1));

    auto const r = e.transformed(tg::scaling_transform2f::make_scaling(tg::vec2f(2, 5)));
    static_assert(std::is_same_v<decltype(r), tg::ellipsoid2f const>);

    CHECK(tgtest::approx(r.center, tg::pos2f(2, 0), 1e-4f));
    CHECK(tgtest::approx(r.semi_axes[0], tg::vec2f(4, 0), 1e-4f));
    CHECK(tgtest::approx(r.semi_axes[1], tg::vec2f(0, 5), 1e-4f));
}

TEST("tg ellipsoid - an ellipse embedded in 3D stays 2D")
{
    // the flat is the xy-plane; a rotation about x tilts it into the xz-plane, and the object dimension never changes
    auto const e = tg::ellipsoid2in3f(tg::pos3f(0, 0, 0), tg::vec3f(2, 0, 0), tg::vec3f(0, 1, 0));

    auto const t = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_x(tg::angle_f::make_from_degree(90)));
    auto const r = e.transformed(t);
    static_assert(std::is_same_v<decltype(r), tg::ellipsoid2in3f const>);

    CHECK(tgtest::approx(r.semi_axes[0], tg::vec3f(2, 0, 0), 1e-4f));
    CHECK(tgtest::approx(r.semi_axes[1], tg::vec3f(0, 0, 1), 1e-4f));
}
