#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/transform/compose.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace
{
/// What each linalg type's `transformed` branches on: which class it can widen the transform to.
///
/// The member itself is unconstrained and ends in a static_assert, so asking whether it compiles
/// would trip that assert rather than yield false — probe the branch condition instead.
/// A concept rather than a bare requires-expression: outside a template an unsatisfied requirement
/// is a hard error rather than a false result.
template <class TargetT, class TransformT>
concept widens_to = requires(TransformT const& t) { TargetT(t); };

template <int D, class T>
[[nodiscard]] bool approx_bivec(tg::bivec<D, T> const& a, tg::bivec<D, T> const& b, T eps = T(1e-4))
{
    for (int i = 0; i < tg::bivec<D, T>::num_components; ++i)
        if (!tgtest::approx(a.data[i], b.data[i], eps))
            return false;
    return true;
}
} // namespace

TEST("tg transformed - the narrow classes take the direct branch")
{
    auto const shift = tg::translation_transform3f::make_translation(tg::vec3f(1, 2, 3));

    SECTION("a translation moves a point and nothing else")
    {
        CHECK(tg::pos3f(4, 0, -1).transformed(shift) == tg::pos3f(5, 2, 2));
        CHECK(tg::vec3f(4, 0, -1).transformed(shift) == tg::vec3f(4, 0, -1));
        CHECK(tg::bivec3f::make_from_values(1, 2, 3).transformed(shift) == tg::bivec3f::make_from_values(1, 2, 3));
    }

    SECTION("the identity leaves everything alone")
    {
        auto const id = tg::identity_transform3f();
        CHECK(tg::pos3f(4, 0, -1).transformed(id) == tg::pos3f(4, 0, -1));
        CHECK(tg::vec3f(4, 0, -1).transformed(id) == tg::vec3f(4, 0, -1));
        CHECK(tg::bivec3f::make_from_values(1, 2, 3).transformed(id) == tg::bivec3f::make_from_values(1, 2, 3));
    }

    SECTION("a linear class has no translation to add")
    {
        auto const s = tg::scaling_transform3f::make_scaling(tg::vec3f(2, 3, 4));
        CHECK(tg::pos3f(1, 1, 1).transformed(s) == tg::pos3f(2, 3, 4));
        CHECK(tg::vec3f(1, 1, 1).transformed(s) == tg::vec3f(2, 3, 4));
    }
}

TEST("tg transformed - points pick up the translation, vectors do not")
{
    auto const t = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(90)))
                       .composed(tg::rigid_transform3f::make_translation(tg::vec3f(10, 0, 0)));

    SECTION("a point is rotated and translated")
    {
        CHECK(tgtest::approx((tg::pos3f(0, 0, 0)).transformed(t), tg::pos3f(0, 10, 0), 1e-4f));
    }

    SECTION("a displacement only sees the linear part")
    {
        CHECK(tgtest::approx((tg::vec3f(1, 0, 0)).transformed(t), tg::vec3f(0, 1, 0), 1e-4f));
    }

    SECTION("the difference of two transformed points is the transformed difference")
    {
        auto const a = tg::pos3f(1, 2, 3);
        auto const b = tg::pos3f(-2, 0, 4);
        CHECK(tgtest::approx(a.transformed(t) - b.transformed(t), (a - b).transformed(t), 1e-4f));
    }
}

namespace
{
// vec and bivec stop at the affine branch, which a projective transform cannot reach:
// a free vector has no base point, so it has no projective image
static_assert(!widens_to<tg::affine_transform3f, tg::projective_transform3f>);
static_assert(widens_to<tg::affine_transform3f, tg::rigid_transform3f>);

// a point falls through to the transform, which owns the perspective divide
static_assert(widens_to<tg::projective_transform3f, tg::projective_transform3f>);
static_assert(requires(tg::pos3f const& p, tg::projective_transform3f const& t) { t.apply_pos(p); });
} // namespace

TEST("tg transformed - a vector has no projective image")
{
    // the static_asserts above carry this test; transform the endpoints and subtract instead
    auto const t = tg::projective_transform3f(tg::rigid_transform3f::make_translation(tg::vec3f(1, 2, 3)));
    auto const a = tg::pos3f(4, 0, 0);
    auto const b = tg::pos3f(1, 0, 0);

    CHECK(tgtest::approx(a.transformed(t) - b.transformed(t), a - b, 1e-4f));
}

TEST("tg transformed - bivectors transform by the second exterior power")
{
    auto const a = tg::vec3f(1, 2, 0);
    auto const b = tg::vec3f(0, 1, 3);

    // the defining property: (Aa) ^ (Ab) == (A^2 A)(a ^ b)
    auto const check_wedge = [&](auto const& t)
    {
        auto const lhs = tg::cross(a.transformed(t), b.transformed(t));
        auto const rhs = (tg::cross(a, b)).transformed(t);
        CHECK(approx_bivec(lhs, rhs, 1e-3f));
    };

    SECTION("under a rotation")
    {
        check_wedge(
            tg::rotation_transform3f::make_rotation(tg::quat_f::make_rotation_y(tg::angle_f::make_from_degree(40))));
    }

    SECTION("under a rigid transform, where the translation must not leak in")
    {
        auto const r
            = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_x(tg::angle_f::make_from_degree(25)));
        check_wedge(r.composed(tg::rigid_transform3f::make_translation(tg::vec3f(5, -2, 1))));
    }

    SECTION("under a non-uniform scaling, where a vector would give the wrong answer")
    {
        check_wedge(tg::scaling_transform3f::make_scaling(tg::vec3f(2, 3, 0.5f)));
    }

    SECTION("under a general affine map")
    {
        auto const m = tg::mat3f::make_from_cols(tg::vec3f(1, 0.5f, 0), tg::vec3f(0, 2, 0.25f), tg::vec3f(0.5f, 0, 1));
        check_wedge(tg::affine_transform3f::make_from_linear_mat(m));
    }
}

TEST("tg transformed - a normal is not a vector")
{
    // a plane normal under a non-uniform scaling picks up the INVERSE scale, not the scale
    auto const s = tg::scaling_transform3f::make_scaling(tg::vec3f(2, 1, 1));

    auto const in_plane_a = tg::vec3f(0, 1, 0);
    auto const in_plane_b = tg::vec3f(0, 0, 1);
    auto const normal = tg::cross(in_plane_a, in_plane_b); // the yz-plane, normal along x

    auto const transformed_normal = tg::dual(normal.transformed(s));

    SECTION("it stays orthogonal to the transformed plane")
    {
        CHECK(tgtest::approx(tg::dot(transformed_normal, in_plane_a.transformed(s)), 0.0f));
        CHECK(tgtest::approx(tg::dot(transformed_normal, in_plane_b.transformed(s)), 0.0f));
    }

    SECTION("treating it as a vector would too, here, but the magnitude differs")
    {
        // scaling x by 2 leaves the yz-plane's normal along x, but its magnitude picks up
        // the product of the OTHER axes rather than the x scale
        CHECK(tgtest::approx(transformed_normal.data[0], 1.0f));
    }
}

TEST("tg transformed - 2D")
{
    auto const t = tg::rigid_transform2f::make_rotation(tg::angle_f::make_from_degree(90));

    SECTION("a point rotates in the plane")
    {
        CHECK(tgtest::approx((tg::pos2f(1, 0)).transformed(t), tg::pos2f(0, 1), 1e-4f));
    }

    SECTION("the 2D bivector scales by the determinant")
    {
        auto const s = tg::scaling_transform2f::make_scaling(tg::vec2f(2, 3));
        auto const b = tg::bivec2f(1);
        CHECK(approx_bivec(b.transformed(s), tg::bivec2f(6)));
    }
}
