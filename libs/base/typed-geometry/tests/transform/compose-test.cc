#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/primitives.hh>
#include <typed-geometry/linalg/mat_ops.hh>
#include <typed-geometry/transform/compose.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

#include <type_traits>

namespace
{
// the result class is the join, computed with canonical() rather than a bit union
using rotation_t = tg::rotation_transform3f;
using translation_t = tg::translation_transform3f;
using scaling_t = tg::scaling_transform3f;

static_assert(std::is_same_v<decltype(rotation_t().composed(translation_t())), tg::rigid_transform3f>);
static_assert(std::is_same_v<decltype(translation_t().composed(rotation_t())), tg::rigid_transform3f>);
static_assert(std::is_same_v<decltype(tg::rigid_transform3f().composed(tg::rigid_transform3f())), tg::rigid_transform3f>);
static_assert(
    std::is_same_v<decltype(tg::rigid_transform3f().composed(tg::similarity_transform3f())), tg::similarity_transform3f>);
static_assert(
    std::is_same_v<decltype(tg::similarity_transform3f().composed(tg::affine_transform3f())), tg::affine_transform3f>);
static_assert(
    std::is_same_v<decltype(tg::affine_transform3f().composed(tg::projective_transform3f())), tg::projective_transform3f>);

// a rotation with a per-axis scaling cannot stay a "rotation and a scaling" — it is a general linear map
static_assert(std::is_same_v<decltype(rotation_t().composed(scaling_t())), tg::linear_transform3f>);
static_assert(
    std::is_same_v<decltype(rotation_t().composed(tg::scaling_translation_transform3f())), tg::affine_transform3f>);
// and a similarity plus a per-axis scaling loses the separate scalar too
static_assert(std::is_same_v<decltype(tg::similarity_transform3f().composed(scaling_t())), tg::affine_transform3f>);

// the free spelling is the member, so it agrees on the result class as well
static_assert(std::is_same_v<decltype(tg::compose(rotation_t(), translation_t())), tg::rigid_transform3f>);

// Concepts rather than bare requires-expressions: outside a template an unsatisfied requirement is a
// hard error rather than a false result.
template <class TransformA, class TransformB>
concept composes_with = requires(TransformA const& a, TransformB const& b) { a.composed(b); };

template <class TransformA, class TransformB>
concept multiplies_with = requires(TransformA const& a, TransformB const& b) { a * b; };

// composition is opt-in, and a homogeneous_transform opts in for its own dimension and scalar only
static_assert(composes_with<rotation_t, translation_t>);
static_assert(!composes_with<rotation_t, tg::rigid_transform2f>);
static_assert(!composes_with<rotation_t, tg::rigid_transform3d>);
static_assert(!composes_with<rotation_t, tg::pos3f>);

// there is deliberately no operator*
static_assert(!multiplies_with<rotation_t, translation_t>);

/// A transform that is not a homogeneous_transform and has no `composed`, so tg::compose has to nest it.
struct swap_xy
{
    [[nodiscard]] constexpr tg::pos3f custom_transform(tg::pos3f const& p) const
    {
        return tg::pos3f(p.data[1], p.data[0], p.data[2]);
    }

    [[nodiscard]] constexpr tg::vec3f custom_transform(tg::vec3f const& v) const
    {
        return tg::vec3f(v.data[1], v.data[0], v.data[2]);
    }
};

static_assert(!composes_with<swap_xy, tg::rigid_transform3f>);
static_assert(!composes_with<tg::rigid_transform3f, swap_xy>);

// tg::compose fuses when it can, and nests when it cannot — a compile-time choice, so the return type says which
static_assert(std::is_same_v<decltype(tg::compose(rotation_t(), translation_t())), tg::rigid_transform3f>);
static_assert(std::is_same_v<decltype(tg::compose(swap_xy(), rotation_t())), tg::composed_transform<swap_xy, rotation_t>>);
static_assert(std::is_same_v<decltype(tg::compose(rotation_t(), swap_xy())), tg::composed_transform<rotation_t, swap_xy>>);

// and nesting nests, so composing is total
static_assert(std::is_same_v<decltype(tg::compose(tg::compose(swap_xy(), rotation_t()), translation_t())),
                             tg::composed_transform<tg::composed_transform<swap_xy, rotation_t>, translation_t>>);
} // namespace

TEST("tg compose - matches the matrix product")
{
    auto const r = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_y(tg::angle_f::make_from_degree(35)));
    auto const t = tg::rigid_transform3f::make_translation(tg::vec3f(1, -2, 4));
    auto const s = tg::similarity_transform3f::make_uniform_scaling(2.5f);
    auto const l = tg::affine_transform3f::make_from_linear_mat(
        tg::mat3f::make_from_cols(tg::vec3f(1, 0.5f, 0), tg::vec3f(0, 2, 0), tg::vec3f(0.25f, 0, 1)));

    SECTION("rigid composition")
    {
        CHECK(tgtest::approx(t.composed(r).to_mat(), t.to_mat() * r.to_mat(), 1e-4f));
        CHECK(tgtest::approx(r.composed(t).to_mat(), r.to_mat() * t.to_mat(), 1e-4f));
    }

    SECTION("across classes")
    {
        CHECK(tgtest::approx(s.composed(r).to_mat(), s.to_mat() * r.to_mat(), 1e-4f));
        CHECK(tgtest::approx(l.composed(t).to_mat(), l.to_mat() * t.to_mat(), 1e-4f));
        CHECK(tgtest::approx(r.composed(l).to_mat(), r.to_mat() * l.to_mat(), 1e-4f));
    }

    SECTION("projective composition")
    {
        auto const p = tg::projective_transform3f(r);
        CHECK(tgtest::approx(p.composed(p).to_mat(), p.to_mat() * p.to_mat(), 1e-4f));
    }

    SECTION("the free spelling fuses through the member")
    {
        CHECK(tg::compose(s, r) == s.composed(r));
        CHECK(tg::compose(l, t) == l.composed(t));
    }
}

TEST("tg compose - falls back to a composed_transform when the two cannot fuse")
{
    auto const r = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(90)));
    auto const t = tg::rigid_transform3f::make_translation(tg::vec3f(1, 2, 3));
    auto const rt = r.composed(t);

    SECTION("it applies the inner transform first")
    {
        auto const c = tg::compose(swap_xy(), rt);
        auto const p = tg::pos3f(4, -1, 2);

        CHECK(tgtest::approx(c(p), swap_xy().custom_transform(rt(p)), 1e-4f));
        CHECK(tgtest::approx(c(p), p.transformed(rt).transformed(swap_xy()), 1e-4f));
        CHECK(tgtest::approx(c.transform(p), c(p), 1e-4f));
        CHECK(tgtest::approx(p.transformed(c), c(p), 1e-4f));
    }

    SECTION("and the other way round, where the rigid transform runs last")
    {
        auto const c = tg::compose(rt, swap_xy());
        auto const p = tg::pos3f(4, -1, 2);

        CHECK(tgtest::approx(c(p), rt(swap_xy().custom_transform(p)), 1e-4f));
        CHECK(tgtest::approx(c(p), p.transformed(swap_xy()).transformed(rt), 1e-4f));
    }

    SECTION("a displacement still skips the translation, because the parts decide")
    {
        auto const c = tg::compose(swap_xy(), rt);
        auto const v = tg::vec3f(1, 0, 0);

        // rotating (1,0,0) by 90 degrees about z gives (0,1,0), and swapping x and y gives (1,0,0) back
        CHECK(tgtest::approx(c(v), tg::vec3f(1, 0, 0), 1e-4f));
    }

    SECTION("nesting is what makes composing total")
    {
        auto const c = tg::compose(tg::compose(swap_xy(), r), t);
        auto const p = tg::pos3f(4, -1, 2);

        CHECK(tgtest::approx(c(p), p.transformed(t).transformed(r).transformed(swap_xy()), 1e-4f));
    }

    SECTION("a hand-built composed_transform over two fusable parts matches the fused one")
    {
        // the point of tg::compose is that you never have to: fusing is strictly better where it exists
        auto const nested = tg::composed_transform<tg::rigid_transform3f, tg::rigid_transform3f>(r, t);
        auto const p = tg::pos3f(4, -1, 2);

        CHECK(tgtest::approx(nested(p), rt(p), 1e-4f));
    }
}

TEST("tg compose - applies the right operand first")
{
    // matching quat, where a.compose(b) also applies b first
    auto const r = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(90)));
    auto const t = tg::rigid_transform3f::make_translation(tg::vec3f(1, 0, 0));
    auto const origin = tg::pos3f(0, 0, 0);

    SECTION("translate, then rotate")
    {
        // (1,0,0) rotated by 90 degrees about z
        CHECK(tgtest::approx(origin.transformed(r.composed(t)), tg::pos3f(0, 1, 0), 1e-4f));
    }

    SECTION("rotate, then translate")
    {
        // the origin is fixed by the rotation, so only the translation shows
        CHECK(tgtest::approx(origin.transformed(t.composed(r)), tg::pos3f(1, 0, 0), 1e-4f));
    }

    SECTION("composition agrees with applying the parts in order")
    {
        auto const p = tg::pos3f(2, 3, -1);
        CHECK(tgtest::approx(p.transformed(r.composed(t)), (p.transformed(t)).transformed(r), 1e-4f));
    }
}

TEST("tg compose - the three spellings agree")
{
    // a(b(obj)) == a.composed(b).transform(obj) == obj.transformed(b).transformed(a)
    auto const a = tg::affine_transform3f::make_from_linear_mat(
        tg::mat3f::make_from_cols(tg::vec3f(1, 0.5f, 0), tg::vec3f(0, 2, 0.25f), tg::vec3f(0.5f, 0, 1)));
    auto const b = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(35)))
                       .composed(tg::rigid_transform3f::make_translation(tg::vec3f(1, 2, 3)));
    auto const ab = a.composed(b);

    SECTION("a point, where the translation and the linear part both show")
    {
        auto const p = tg::pos3f(4, -1, 2);
        CHECK(tgtest::approx(a(b(p)), ab.transform(p), 1e-4f));
        CHECK(tgtest::approx(a(b(p)), p.transformed(b).transformed(a), 1e-4f));
    }

    SECTION("a displacement, which the translation must not reach")
    {
        auto const v = tg::vec3f(4, -1, 2);
        CHECK(tgtest::approx(a(b(v)), ab.transform(v), 1e-4f));
        CHECK(tgtest::approx(a(b(v)), v.transformed(b).transformed(a), 1e-4f));
    }

    SECTION("a plane, whose normal goes through the cofactor rather than the linear part")
    {
        auto const pl = tg::plane3f(tg::vec3f(0, 0, 1), 1.0f);
        CHECK(tgtest::approx(a(b(pl)).normal, ab.transform(pl).normal, 1e-4f));
        CHECK(tgtest::approx(a(b(pl)).dist, ab.transform(pl).dist, 1e-4f));
        CHECK(tgtest::approx(a(b(pl)).normal, pl.transformed(b).transformed(a).normal, 1e-4f));
        CHECK(tgtest::approx(a(b(pl)).dist, pl.transformed(b).transformed(a).dist, 1e-4f));
    }

    SECTION("and where composing widens the class, so does the result type")
    {
        // similarity o scaling is affine, under which a sphere is an ellipsoid — which is what the chain returns too
        auto const scale = tg::similarity_transform3f::make_uniform_scaling(2.0f);
        auto const squash = tg::scaling_transform3f::make_scaling(tg::vec3f(1, 2, 3));
        auto const s = tg::sphere3f(tg::pos3f(1, 0, 0), 2.0f);

        static_assert(std::is_same_v<decltype(scale.composed(squash)), tg::affine_transform3f>);
        static_assert(std::is_same_v<decltype(scale(squash(s))), tg::ellipsoid3f>);
        static_assert(std::is_same_v<decltype(scale.composed(squash).transform(s)), tg::ellipsoid3f>);
        static_assert(std::is_same_v<decltype(s.transformed(squash).transformed(scale)), tg::ellipsoid3f>);

        auto const nested = scale(squash(s));
        auto const composed = scale.composed(squash).transform(s);
        auto const chained = s.transformed(squash).transformed(scale);

        CHECK(tgtest::approx(nested.center, composed.center, 1e-4f));
        CHECK(tgtest::approx(nested.center, chained.center, 1e-4f));
        for (int i = 0; i < 3; ++i)
        {
            CHECK(tgtest::approx(nested.semi_axes[i], composed.semi_axes[i], 1e-4f));
            CHECK(tgtest::approx(nested.semi_axes[i], chained.semi_axes[i], 1e-4f));
        }
    }
}

TEST("tg compose - associativity and identity")
{
    auto const r = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_x(tg::angle_f::make_from_degree(20)));
    auto const t = tg::rigid_transform3f::make_translation(tg::vec3f(3, 1, 2));
    auto const s = tg::similarity_transform3f::make_uniform_scaling(1.5f);

    SECTION("associative")
    {
        CHECK(tgtest::approx(s.composed(r).composed(t).to_mat(), s.composed(r.composed(t)).to_mat(), 1e-4f));
    }

    SECTION("the identity is neutral")
    {
        CHECK(tgtest::approx(r.composed(tg::rigid_transform3f::identity).to_mat(), r.to_mat(), 1e-5f));
        CHECK(tgtest::approx(tg::rigid_transform3f::identity.composed(r).to_mat(), r.to_mat(), 1e-5f));
    }
}

TEST("tg compose - 2D")
{
    auto const r = tg::rigid_transform2f::make_rotation(tg::angle_f::make_from_degree(45));
    auto const t = tg::rigid_transform2f::make_translation(tg::vec2f(1, 0));

    SECTION("two 45 degree rotations make a 90")
    {
        CHECK(tgtest::approx((tg::vec2f(1, 0)).transformed(r.composed(r)), tg::vec2f(0, 1), 1e-4f));
    }

    SECTION("matches the matrix product")
    {
        CHECK(tgtest::approx(r.composed(t).to_mat(), r.to_mat() * t.to_mat(), 1e-4f));
    }
}
