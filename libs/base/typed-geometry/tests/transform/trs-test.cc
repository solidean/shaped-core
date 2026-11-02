#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/transform/compose.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

#include <type_traits>

namespace
{
auto const k_t = tg::vec3f(1, -2, 3);
auto const k_q = tg::quat_f::make_rotation_axis_angle(tg::vec3f(1, 1, 0).normalized(), tg::angle_f::make_from_degree(70));
auto const k_s = tg::vec3f(2, 0.5f, -3); // non-uniform, and mirrored on z
auto const k_points = {tg::pos3f(0, 0, 0), tg::pos3f(1, 2, 3), tg::pos3f(-4, 0.5f, 7)};
} // namespace

TEST("tg transform - make_trs applies scale, then rotation, then translation")
{
    auto const trs = tg::affine_transform3f::make_trs(k_t, k_q, k_s);
    for (auto const p : k_points)
    {
        auto const scaled = tg::vec3f(p.data[0] * k_s.data[0], p.data[1] * k_s.data[1], p.data[2] * k_s.data[2]);
        CHECK(tgtest::approx(trs(p), tg::pos3f::zero + k_q * scaled + k_t));
    }
}

TEST("tg transform - make_trs is the composition in that order, in any class that holds it")
{
    auto const composed
        = tg::compose(tg::translation_transform3f::make_translation(k_t), tg::rotation_transform3f::make_rotation(k_q),
                      tg::signed_scaling_transform3f::make_scaling(k_s));
    static_assert(std::is_same_v<decltype(composed), tg::affine_transform3f const>);

    auto const affine = tg::affine_transform3f::make_trs(k_t, k_q, k_s);
    auto const projective = tg::projective_transform3f::make_trs(k_t, k_q, k_s);
    CHECK(tgtest::approx(affine.to_mat(), composed.to_mat()));
    CHECK(tgtest::approx(projective.to_mat(), composed.to_mat()));
}

TEST("tg transform - an affine transform from a matrix")
{
    auto const trs = tg::affine_transform3f::make_trs(k_t, k_q, k_s);

    SECTION("a round trip through the matrix is the same map")
    {
        auto const back = tg::affine_transform3f::make_from_mat(trs.to_mat());
        for (auto const p : k_points)
            CHECK(tgtest::approx(back(p), trs(p)));
        CHECK(tgtest::approx(back(tg::vec3f(1, 0, 0)), trs(tg::vec3f(1, 0, 0))));
    }
    SECTION("a matrix whose last row is not (0, 0, 0, 1) is refused")
    {
        auto m = trs.to_mat();
        m[0, 3] = 0.25f;
        CHECK_ASSERTS(tg::affine_transform3f::make_from_mat(m));
    }
}

TEST("tg transform - compose applies its last argument first")
{
    auto const a = tg::translation_transform3f::make_translation(tg::vec3f(0, 0, 1));
    auto const b
        = tg::rotation_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(90)));
    auto const c = tg::scaling_transform3f::make_scaling(tg::vec3f(2, 3, 4));
    auto const d = tg::translation_transform3f::make_translation(tg::vec3f(5, 0, 0));
    for (auto const p : k_points)
    {
        CHECK(tgtest::approx(tg::compose(a, b, c)(p), a(b(c(p)))));
        CHECK(tgtest::approx(tg::compose(a, b, c, d)(p), a(b(c(d(p))))));
    }
}
