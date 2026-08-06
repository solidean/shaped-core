#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/transform/compose.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

#include <type_traits>

namespace tc = tg::impl::transform_class;

static_assert(std::is_trivially_copyable_v<tg::rigid_transform3f>, "a transform should be trivially copyable");
static_assert(std::is_trivially_copyable_v<tg::affine_transform3f>, "a transform should be trivially copyable");
static_assert(std::is_trivially_copyable_v<tg::projective_transform3f>, "a transform should be trivially copyable");
static_assert(std::is_standard_layout_v<tg::similarity_transform3f>,
              "a transform holds exactly one member and stays standard-layout");

// the representation really is chosen from the flags, not a matrix everywhere
static_assert(sizeof(tg::identity_transform3f) == 1);
static_assert(sizeof(tg::translation_transform3f) == 3 * sizeof(float), "a pure translation is just a vector");
static_assert(sizeof(tg::rotation_transform3f) == 4 * sizeof(float));
static_assert(sizeof(tg::rigid_transform3f) == 7 * sizeof(float));
static_assert(sizeof(tg::similarity_transform3f) == 8 * sizeof(float));
static_assert(sizeof(tg::affine_transform3f) == 12 * sizeof(float));
static_assert(sizeof(tg::projective_transform3f) == 16 * sizeof(float));
static_assert(sizeof(tg::rotation_transform2f) == 2 * sizeof(float), "a 2D rotation is a unit complex number");

//
// A transform carries a source and a target dimension, so it can eventually lift and project between spaces.
// Only the square case exists today, and every named alias is square.
//
static_assert(std::is_same_v<tg::rigid_transform3f, tg::homogeneous_transform<3, 3, float, tc::rigid>>);
static_assert(std::is_same_v<tg::affine_transform<2, double>, tg::homogeneous_transform<2, 2, double, tc::affine>>);
static_assert(tg::rigid_transform3f::source_dimension == 3);
static_assert(tg::rigid_transform3f::target_dimension == 3);
static_assert(tg::rotation_transform2f::source_dimension == 2);

// the shapes the two dimensions pin down: a source vector in, a target vector out
static_assert(std::is_same_v<decltype(tg::affine_transform3f().linear_mat()), tg::mat<3, 3, float>>);
static_assert(std::is_same_v<decltype(tg::affine_transform3f().to_mat()), tg::mat<4, 4, float>>);
static_assert(std::is_same_v<decltype(tg::affine_transform3f().transform(tg::pos3f())), tg::pos3f>);
static_assert(std::is_same_v<decltype(tg::affine_transform3f().transform(tg::vec3f())), tg::vec3f>);
static_assert(std::is_same_v<decltype(tg::rigid_transform3f().translation()), tg::vec3f>);

// widening is explicit, so the ladder can rank registrations
static_assert(std::is_constructible_v<tg::affine_transform3f, tg::rigid_transform3f>);
static_assert(!std::is_convertible_v<tg::rigid_transform3f, tg::affine_transform3f>);
static_assert(std::is_constructible_v<tg::affine_transform3f, tg::similarity_transform3f>,
              "a similarity is affine even though its bits are not a subset");
// narrowing is not a constructor at all
static_assert(!std::is_constructible_v<tg::rigid_transform3f, tg::affine_transform3f>);

TEST("tg homogeneous_transform - default is the identity")
{
    auto const p = tg::pos3f(1, 2, 3);

    SECTION("every storage kind defaults to the identity, not to zero")
    {
        CHECK(tgtest::approx(p.transformed(tg::identity_transform3f()), p));
        CHECK(tgtest::approx(p.transformed(tg::translation_transform3f()), p));
        CHECK(tgtest::approx(p.transformed(tg::rotation_transform3f()), p));
        CHECK(tgtest::approx(p.transformed(tg::scaling_transform3f()), p));
        CHECK(tgtest::approx(p.transformed(tg::rigid_transform3f()), p));
        CHECK(tgtest::approx(p.transformed(tg::similarity_transform3f()), p));
        CHECK(tgtest::approx(p.transformed(tg::affine_transform3f()), p));
        CHECK(tgtest::approx(p.transformed(tg::projective_transform3f()), p));
    }

    SECTION("the identity constant equals a default-constructed transform")
    {
        CHECK(tg::rigid_transform3f::identity == tg::rigid_transform3f());
        CHECK(tg::affine_transform3f::identity == tg::affine_transform3f());
    }

    SECTION("the identity matrix is the identity")
    {
        CHECK(tg::rigid_transform3f().to_mat() == tg::mat4f::identity);
        CHECK(tg::affine_transform3f().to_mat() == tg::mat4f::identity);
        CHECK(tg::projective_transform3f().to_mat() == tg::mat4f::identity);
    }
}

TEST("tg homogeneous_transform - factories and accessors")
{
    SECTION("translation")
    {
        auto const t = tg::rigid_transform3f::make_translation(tg::vec3f(1, 2, 3));
        CHECK(t.translation() == tg::vec3f(1, 2, 3));
        CHECK(tgtest::approx((tg::pos3f(0, 0, 0)).transformed(t), tg::pos3f(1, 2, 3)));
    }

    SECTION("uniform scaling, including a negative factor")
    {
        auto const s = tg::signed_similarity_transform3f::make_uniform_scaling(-2.0f);
        CHECK(s.uniform_scale() == -2.0f);
        CHECK(tgtest::approx((tg::pos3f(1, 2, 3)).transformed(s), tg::pos3f(-2, -4, -6)));
    }

    SECTION("non-uniform scaling")
    {
        auto const s = tg::scaling_transform3f::make_scaling(tg::vec3f(2, 3, 4));
        CHECK(s.scale() == tg::vec3f(2, 3, 4));
        CHECK(tgtest::approx((tg::pos3f(1, 1, 1)).transformed(s), tg::pos3f(2, 3, 4)));
    }

    SECTION("3D rotation from a quaternion")
    {
        auto const q = tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(90));
        auto const r = tg::rigid_transform3f::make_rotation(q);
        CHECK(r.rotation() == q);
        CHECK(tgtest::approx((tg::vec3f(1, 0, 0)).transformed(r), tg::vec3f(0, 1, 0)));
    }

    SECTION("2D rotation from an angle")
    {
        auto const r = tg::rigid_transform2f::make_rotation(tg::angle_f::make_from_degree(90));
        CHECK(tgtest::approx((tg::vec2f(1, 0)).transformed(r), tg::vec2f(0, 1)));
        CHECK(tgtest::approx(r.rotation().degree(), 90.0f, 1e-3f));
    }

    SECTION("an arbitrary linear map")
    {
        auto const m = tg::mat3f::make_from_cols(tg::vec3f(1, 2, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, 1));
        auto const l = tg::affine_transform3f::make_from_linear_mat(m);
        CHECK(l.linear_mat() == m);
        CHECK(tgtest::approx((tg::vec3f(1, 0, 0)).transformed(l), tg::vec3f(1, 2, 0)));
    }
}

TEST("tg homogeneous_transform - a positive class rejects a non-positive factor")
{
    // the whole point of the flag: without negative_scaling the factors are a promise the
    // factories enforce, which is what lets aabb and sphere skip their sign handling
    SECTION("uniform")
    {
        CHECK_ASSERTS(tg::similarity_transform3f::make_uniform_scaling(-2.0f));
        CHECK_ASSERTS(tg::similarity_transform3f::make_uniform_scaling(0.0f));
    }

    SECTION("per axis")
    {
        CHECK_ASSERTS(tg::scaling_transform3f::make_scaling(tg::vec3f(1, -1, 1)));
    }

    SECTION("the signed classes accept both")
    {
        CHECK(tg::signed_similarity_transform3f::make_uniform_scaling(-2.0f).uniform_scale() == -2.0f);
        CHECK(tg::signed_scaling_transform3f::make_scaling(tg::vec3f(1, -1, 1)).scale() == tg::vec3f(1, -1, 1));
    }
}

TEST("tg homogeneous_transform - widening")
{
    auto const q = tg::quat_f::make_rotation_y(tg::angle_f::make_from_degree(30));
    auto const r
        = tg::rigid_transform3f::make_rotation(q).composed(tg::rigid_transform3f::make_translation(tg::vec3f(1, 2, 3)));
    auto const p = tg::pos3f(4, -1, 2);

    SECTION("widening preserves the map exactly")
    {
        CHECK(tgtest::approx(p.transformed(tg::affine_transform3f(r)), p.transformed(r), 1e-4f));
        CHECK(tgtest::approx(p.transformed(tg::projective_transform3f(r)), p.transformed(r), 1e-4f));
    }

    SECTION("widening preserves the matrix")
    {
        CHECK(tgtest::approx(tg::affine_transform3f(r).to_mat(), r.to_mat(), 1e-5f));
        CHECK(tgtest::approx(tg::projective_transform3f(r).to_mat(), r.to_mat(), 1e-5f));
    }

    SECTION("a similarity widens to an affine transform despite the cleared uniform_scaling bit")
    {
        auto const s = tg::similarity_transform3f::make_uniform_scaling(3.0f);
        CHECK(tgtest::approx(p.transformed(tg::affine_transform3f(s)), p.transformed(s), 1e-4f));
    }

    SECTION("a uniform scaling widens into a per-axis one")
    {
        auto const u = tg::homogeneous_transform<3, 3, float, tc::uniform_scaling>::make_uniform_scaling(2.0f);
        CHECK(tg::scaling_transform3f(u).scale() == tg::vec3f(2, 2, 2));
    }
}

TEST("tg homogeneous_transform - to_mat")
{
    SECTION("the translation sits in the last column")
    {
        auto const t = tg::rigid_transform3f::make_translation(tg::vec3f(1, 2, 3));
        auto const m = t.to_mat();
        CHECK((m[3, 0]) == 1);
        CHECK((m[3, 1]) == 2);
        CHECK((m[3, 2]) == 3);
        CHECK((m[3, 3]) == 1);
    }

    SECTION("the linear block matches linear_mat")
    {
        auto const q = tg::quat_f::make_rotation_x(tg::angle_f::make_from_degree(20));
        auto const r = tg::similarity_transform3f::make_rotation(q).composed(
            tg::similarity_transform3f::make_uniform_scaling(2.0f));
        auto const l = r.linear_mat();
        auto const m = r.to_mat();

        for (int c = 0; c < 3; ++c)
            for (int rr = 0; rr < 3; ++rr)
                CHECK(tgtest::approx((m[c, rr]), (l[c, rr])));
    }
}

// the representation is private, so tg::impl::transform_representation_of is the only way in.
// That it is unreachable is not expressible as a static_assert: the access failure is a hard error, not a
// substitution failure, so a requires-expression naming it does not compile at all.
TEST("tg homogeneous_transform - the impl representation accessor")
{
    SECTION("it hands back the members the public accessors are derived from")
    {
        auto const q = tg::quat_f::make_rotation_y(tg::angle_f::make_from_degree(40));
        auto const t = tg::rigid_transform3f::make_translation(tg::vec3f(1, -2, 4))
                           .composed(tg::rigid_transform3f::make_rotation(q));

        auto const& r = tg::impl::transform_representation_of(t);
        CHECK(r.translation == t.translation());
        CHECK(r.linear.rot.to_quat() == t.rotation());
    }

    SECTION("it aliases the transform rather than copying it")
    {
        auto const t = tg::similarity_transform3f::make_uniform_scaling(3.0f);
        auto const& r = tg::impl::transform_representation_of(t);
        CHECK(r.linear.scale == t.uniform_scale());
        CHECK(static_cast<void const*>(&r) == static_cast<void const*>(&t));
    }

    SECTION("a projective transform hands back its matrix")
    {
        auto const m = tg::mat4f::make_from_cols(tg::vec4f(1, 0, 0, 0), tg::vec4f(0, 1, 0, 0), tg::vec4f(0, 0, 1, -1),
                                                 tg::vec4f(0, 0, 0, 2));
        auto const p = tg::projective_transform3f::make_from_mat(m);
        CHECK(tg::impl::transform_representation_of(p).m == m);
    }
}
