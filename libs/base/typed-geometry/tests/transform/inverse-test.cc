#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/transform/compose.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>
#include <typed-geometry/transform/inverse.hh>

#include <type_traits>

namespace tc = tg::transform_class;

namespace
{
// every canonical class is closed under inversion, so the class never widens
static_assert(std::is_same_v<decltype(tg::inverse(tg::rigid_transform3f())), tg::rigid_transform3f>);
static_assert(std::is_same_v<decltype(tg::inverse(tg::similarity_transform3f())), tg::similarity_transform3f>);
static_assert(std::is_same_v<decltype(tg::inverse(tg::affine_transform3f())), tg::affine_transform3f>);
static_assert(std::is_same_v<decltype(tg::inverse(tg::projective_transform3f())), tg::projective_transform3f>);
static_assert(std::is_same_v<decltype(tg::inverse(tg::scaling_transform3f())), tg::scaling_transform3f>);
} // namespace

TEST("tg inverse - round trips to the identity")
{
    auto const p = tg::pos3f(2, -3, 5);

    auto const check_round_trip = [&](auto const& t)
    {
        auto const inv = tg::inverse(t);
        CHECK(tgtest::approx((p.transformed(t)).transformed(inv), p, 1e-3f));
        CHECK(tgtest::approx((p.transformed(inv)).transformed(t), p, 1e-3f));
        CHECK(tgtest::approx(t.composed(inv).to_mat(), tg::mat4f::identity, 1e-3f));
    };

    SECTION("translation")
    {
        check_round_trip(tg::translation_transform3f::make_translation(tg::vec3f(1, 2, 3)));
    }

    SECTION("rotation")
    {
        check_round_trip(
            tg::rotation_transform3f::make_rotation(tg::quat_f::make_rotation_y(tg::angle_f::make_from_degree(50))));
    }

    SECTION("non-uniform scaling")
    {
        check_round_trip(tg::signed_scaling_transform3f::make_scaling(tg::vec3f(2, -3, 0.5f)));
    }

    SECTION("rigid")
    {
        auto const r
            = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(-70)));
        check_round_trip(r.composed(tg::rigid_transform3f::make_translation(tg::vec3f(4, 0, -1))));
    }

    SECTION("similarity, including a negative scale")
    {
        auto const r = tg::signed_similarity_transform3f::make_rotation(
            tg::quat_f::make_rotation_x(tg::angle_f::make_from_degree(15)));
        auto const s = tg::signed_similarity_transform3f::make_uniform_scaling(-2.0f);
        check_round_trip(r.composed(s).composed(tg::signed_similarity_transform3f::make_translation(tg::vec3f(1, 1, 1))));
    }

    SECTION("affine")
    {
        auto const m = tg::mat3f::make_from_cols(tg::vec3f(1, 0.5f, 0), tg::vec3f(0, 2, 0.25f), tg::vec3f(0.5f, 0, 1));
        check_round_trip(tg::affine_transform3f::make_from_linear_mat(m).composed(
            tg::affine_transform3f::make_translation(tg::vec3f(2, 3, 4))));
    }

    SECTION("projective")
    {
        auto const r
            = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_y(tg::angle_f::make_from_degree(25)));
        check_round_trip(
            tg::projective_transform3f(r.composed(tg::rigid_transform3f::make_translation(tg::vec3f(0, 1, 6)))));
    }

    SECTION("2D rigid")
    {
        auto const r = tg::rigid_transform2f::make_rotation(tg::angle_f::make_from_degree(33));
        auto const t = r.composed(tg::rigid_transform2f::make_translation(tg::vec2f(2, -1)));
        auto const q = tg::pos2f(3, 4);
        CHECK(tgtest::approx((q.transformed(t)).transformed(tg::inverse(t)), q, 1e-3f));
    }
}

TEST("tg inverse - a rigid inverse is the transposed rotation")
{
    auto const q = tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(60));
    auto const t
        = tg::rigid_transform3f::make_rotation(q).composed(tg::rigid_transform3f::make_translation(tg::vec3f(1, 2, 3)));
    auto const inv = tg::inverse(t);

    SECTION("the rotation inverts to its conjugate")
    {
        CHECK(tgtest::approx(inv.rotation().data[2], -q.data[2]));
        CHECK(tgtest::approx(inv.rotation().data[3], q.data[3]));
    }

    SECTION("the translation becomes -R^-1 t")
    {
        CHECK(tgtest::approx(inv.translation(), -inv.apply_linear(t.translation()), 1e-4f));
    }
}

TEST("tg inverse - a zero scale asserts")
{
    // a positive class rejects zero at construction, so the singular case only reaches inverse
    // through a signed one
    SECTION("uniform")
    {
        auto const s = tg::signed_similarity_transform3f::make_uniform_scaling(0.0f);
        CHECK_ASSERTS(tg::inverse(s));
    }

    SECTION("per axis")
    {
        auto const s = tg::signed_scaling_transform3f::make_scaling(tg::vec3f(1, 0, 1));
        CHECK_ASSERTS(tg::inverse(s));
    }
}

TEST("tg inverse - a singular matrix part degrades to zero")
{
    // tg::inverse(mat) does not assert, so the matrix-backed classes inherit the zero matrix
    SECTION("linear")
    {
        auto const m = tg::mat3f::make_from_cols(tg::vec3f(1, 2, 3), tg::vec3f(2, 4, 6), tg::vec3f(0, 1, 0));
        auto const l = tg::affine_transform3f::make_from_linear_mat(m);
        CHECK(tg::inverse(l).linear_mat() == tg::mat3f::zero);
    }

    SECTION("projective")
    {
        auto const p = tg::projective_transform3f::make_from_mat(tg::mat4f::zero);
        CHECK(tg::inverse(p).to_mat() == tg::mat4f::zero);
    }
}
