#include "../approx.hh"

#include <nexus/test.hh>
#include <typed-geometry/geometry/primitives/primitives.hh>
#include <typed-geometry/transform/compose.hh>

#include <type_traits>

namespace
{
//
// The return type is decided by the object AND the transform class together.
//

// a sphere stays a sphere exactly as long as angles are preserved
static_assert(std::is_same_v<decltype(tg::sphere3f().transformed(tg::identity_transform3f())), tg::sphere3f>);
static_assert(std::is_same_v<decltype(tg::sphere3f().transformed(tg::translation_transform3f())), tg::sphere3f>);
static_assert(std::is_same_v<decltype(tg::sphere3f().transformed(tg::rotation_transform3f())), tg::sphere3f>);
static_assert(std::is_same_v<decltype(tg::sphere3f().transformed(tg::rigid_transform3f())), tg::sphere3f>);
static_assert(std::is_same_v<decltype(tg::sphere3f().transformed(tg::similarity_transform3f())), tg::sphere3f>);

// ... and becomes an ellipsoid the moment it does not (3D only — the 2D image is an ellipse, which tg lacks)
static_assert(std::is_same_v<decltype(tg::sphere3f().transformed(tg::scaling_transform3f())), tg::ellipsoid3f>);
static_assert(std::is_same_v<decltype(tg::sphere3f().transformed(tg::linear_transform3f())), tg::ellipsoid3f>);
static_assert(std::is_same_v<decltype(tg::sphere3f().transformed(tg::affine_transform3f())), tg::ellipsoid3f>);

static_assert(std::is_same_v<decltype(tg::ellipsoid3f().transformed(tg::rigid_transform3f())), tg::ellipsoid3f>);

// the other primitives keep their type
static_assert(std::is_same_v<decltype(tg::triangle3f().transformed(tg::affine_transform3f())), tg::triangle3f>);
static_assert(std::is_same_v<decltype(tg::segment3f().transformed(tg::rigid_transform3f())), tg::segment3f>);
static_assert(std::is_same_v<decltype(tg::ray3f().transformed(tg::similarity_transform3f())), tg::ray3f>);
static_assert(std::is_same_v<decltype(tg::plane3f().transformed(tg::rigid_transform3f())), tg::plane3f>);
static_assert(std::is_same_v<decltype(tg::aabb3f().transformed(tg::scaling_translation_transform3f())), tg::aabb3f>);

//
// What is deliberately NOT supported.
//
// Calling those is a static_assert, so the branch conditions are probed instead: a class the
// transform cannot widen to is exactly why the object's chain falls through to the failure case.
// Concepts rather than bare requires-expressions — outside a template an unsatisfied requirement
// is a hard error rather than a false result.
//

template <class TargetT, class TransformT>
concept widens_to = requires(TransformT const& t) { TargetT(t); };

// an aabb only survives the axis-aligned classes; a rotated one is an oriented box, which tg lacks
static_assert(widens_to<tg::scaling_translation_transform3f, tg::translation_transform3f>);
static_assert(widens_to<tg::scaling_translation_transform3f, tg::scaling_transform3f>);
static_assert(widens_to<tg::scaling_translation_transform3f, tg::scaling_translation_transform3f>);
static_assert(!widens_to<tg::scaling_translation_transform3f, tg::rotation_transform3f>);
static_assert(!widens_to<tg::scaling_translation_transform3f, tg::rigid_transform3f>);
static_assert(!widens_to<tg::scaling_translation_transform3f, tg::affine_transform3f>);

// a projective transform widens to nothing narrower, which is what stops a ray, a line and a sphere there
static_assert(!widens_to<tg::affine_transform3f, tg::projective_transform3f>);
static_assert(!widens_to<tg::similarity_transform3f, tg::projective_transform3f>);

// the similarity view is what keeps a sphere a sphere, and it stops at the general linear classes
static_assert(widens_to<tg::similarity_transform3f, tg::rigid_transform3f>);
static_assert(!widens_to<tg::similarity_transform3f, tg::scaling_transform3f>);
static_assert(!widens_to<tg::similarity_transform3f, tg::affine_transform3f>);

// and an unrelated type widens to nothing, so every object's chain rejects it
struct not_a_transform
{
};
static_assert(!widens_to<tg::affine_transform3f, not_a_transform>);
} // namespace

TEST("tg transformed - a sphere under a similarity is still a sphere")
{
    auto const s = tg::sphere3f(tg::pos3f(1, 0, 0), 2.0f);

    SECTION("a rigid transform moves it without changing the radius")
    {
        auto const t
            = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(90)))
                  .composed(tg::rigid_transform3f::make_translation(tg::vec3f(0, 0, 5)));
        auto const r = s.transformed(t);

        CHECK(tgtest::approx(r.radius, 2.0f));
        CHECK(tgtest::approx(r.center, s.center.transformed(t), 1e-4f));
    }

    SECTION("a uniform scaling scales the radius")
    {
        auto const r = s.transformed(tg::similarity_transform3f::make_uniform_scaling(3.0f));
        CHECK(tgtest::approx(r.radius, 6.0f));
    }

    SECTION("a negative uniform scaling takes the magnitude")
    {
        // in 3D a negative uniform scale is a reflection, which still maps spheres to spheres
        auto const r = s.transformed(tg::signed_similarity_transform3f::make_uniform_scaling(-3.0f));
        CHECK(tgtest::approx(r.radius, 6.0f));
        CHECK(tgtest::approx(r.center, tg::pos3f(-3, 0, 0), 1e-4f));
    }
}

TEST("tg transformed - a sphere under a non-uniform scaling becomes an ellipsoid")
{
    auto const s = tg::sphere3f(tg::pos3f(0, 0, 0), 2.0f);
    auto const e = s.transformed(tg::scaling_transform3f::make_scaling(tg::vec3f(1, 2, 3)));

    SECTION("the semi-axes carry the radius and the scaling")
    {
        CHECK(tgtest::approx(e.semi_axes[0], tg::vec3f(2, 0, 0), 1e-4f));
        CHECK(tgtest::approx(e.semi_axes[1], tg::vec3f(0, 4, 0), 1e-4f));
        CHECK(tgtest::approx(e.semi_axes[2], tg::vec3f(0, 0, 6), 1e-4f));
    }

    SECTION("its center is where the sphere's center went")
    {
        CHECK(tgtest::approx(e.center, tg::pos3f(0, 0, 0), 1e-4f));
    }
}

TEST("tg transformed - an aabb takes the axis-aligned classes")
{
    // every narrower class reaches the same branch by widening to scaling_translation_transform
    auto const b = tg::aabb3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 2, 3));

    SECTION("a pure translation")
    {
        auto const r = b.transformed(tg::translation_transform3f::make_translation(tg::vec3f(1, 1, 1)));
        CHECK(r.min == tg::pos3f(1, 1, 1));
        CHECK(r.max == tg::pos3f(2, 3, 4));
    }

    SECTION("a per-axis scaling")
    {
        auto const r = b.transformed(tg::scaling_transform3f::make_scaling(tg::vec3f(2, 2, 2)));
        CHECK(r.min == tg::pos3f(0, 0, 0));
        CHECK(r.max == tg::pos3f(2, 4, 6));
    }

    SECTION("a negative factor re-sorts the corners")
    {
        auto const r = b.transformed(tg::signed_scaling_transform3f::make_scaling(tg::vec3f(-1, 1, 1)));
        CHECK(r.min == tg::pos3f(-1, 0, 0));
        CHECK(r.max == tg::pos3f(0, 2, 3));
    }
}

TEST("tg transformed - the other primitives")
{
    auto const t = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(90)))
                       .composed(tg::rigid_transform3f::make_translation(tg::vec3f(1, 0, 0)));

    SECTION("a triangle transforms vertex by vertex")
    {
        auto const tri = tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0));
        auto const r = tri.transformed(t);

        CHECK(tgtest::approx(r.pos0, tri.pos0.transformed(t), 1e-4f));
        CHECK(tgtest::approx(r.pos1, tri.pos1.transformed(t), 1e-4f));
        CHECK(tgtest::approx(r.pos2, tri.pos2.transformed(t), 1e-4f));
    }

    SECTION("a ray moves its origin and rotates its direction")
    {
        auto const r = tg::ray3f(tg::pos3f(0, 0, 0), tg::vec3f(1, 0, 0)).transformed(t);
        CHECK(tgtest::approx(r.origin, tg::pos3f(0, 1, 0), 1e-4f));
        CHECK(tgtest::approx(r.dir, tg::vec3f(0, 1, 0), 1e-4f));
    }

    SECTION("a segment keeps its endpoints")
    {
        auto const s = tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(2, 0, 0)).transformed(t);
        CHECK(tgtest::approx(s.pos0, tg::pos3f(0, 1, 0), 1e-4f));
        CHECK(tgtest::approx(s.pos1, tg::pos3f(0, 3, 0), 1e-4f));
    }
}

TEST("tg transformed - a plane's normal picks up the inverse transpose")
{
    // the xy-plane through z == 1
    auto const p = tg::plane3f(tg::vec3f(0, 0, 1), 1.0f);

    SECTION("a rigid transform rotates the normal")
    {
        auto const t
            = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_x(tg::angle_f::make_from_degree(90)));
        auto const r = p.transformed(t);

        // +z rotates to -y under a +90 degree rotation about x
        CHECK(tgtest::approx(r.normal, tg::vec3f(0, -1, 0), 1e-4f));
        CHECK(tgtest::approx(r.dist, 1.0f, 1e-4f));
    }

    SECTION("a non-uniform scaling is where a plain vector would be wrong")
    {
        // scaling z by 2 moves the plane to z == 2 but leaves its normal along z
        auto const t = tg::affine_transform3f(tg::scaling_transform3f::make_scaling(tg::vec3f(3, 5, 2)));
        auto const r = p.transformed(t);

        CHECK(tgtest::approx(r.normal, tg::vec3f(0, 0, 1), 1e-4f));
        CHECK(tgtest::approx(r.dist, 2.0f, 1e-4f));
    }

    SECTION("points on the plane stay on the transformed plane")
    {
        auto const m = tg::mat3f::make_from_cols(tg::vec3f(1, 0.5f, 0), tg::vec3f(0, 2, 0.25f), tg::vec3f(0.5f, 0, 1));
        auto const t = tg::affine_transform3f::make_from_linear_mat(m).composed(
            tg::affine_transform3f::make_translation(tg::vec3f(1, 2, 3)));
        auto const r = p.transformed(t);

        for (auto const& q : {tg::pos3f(0, 0, 1), tg::pos3f(3, -2, 1), tg::pos3f(-1, 4, 1)})
            CHECK(tgtest::approx(tg::dot(r.normal, q.transformed(t) - tg::pos3f(0, 0, 0)), r.dist, 1e-3f));
    }
}

namespace
{
/// A transform special-cases an object through a PRIVATE custom_transform plus a friend declaration.
///
/// Access is part of the requires-expression, so an object that was not befriended simply does not
/// see the branch and falls through to the normal chain — here, sphere is special-cased and
/// segment is not.
struct sphere_squasher
{
private:
    template <int D, class T>
    friend struct tg::sphere;

    [[nodiscard]] tg::sphere3f custom_transform(tg::sphere3f const& s) const
    {
        return tg::sphere3f(s.center, s.radius * 0.5f);
    }
};

template <class ObjT, class TransformT>
concept has_custom_transform = requires(ObjT const& o, TransformT const& t) { t.custom_transform(o); };

// only the befriended object can reach it — from outside, the member may as well not exist
static_assert(!has_custom_transform<tg::sphere3f, sphere_squasher>);
} // namespace

TEST("tg transformed - a transform may special-case an object")
{
    auto const r = tg::sphere3f(tg::pos3f(1, 2, 3), 4.0f).transformed(sphere_squasher());

    SECTION("the special case wins over every other branch")
    {
        CHECK(r.center == tg::pos3f(1, 2, 3));
        CHECK(tgtest::approx(r.radius, 2.0f));
    }
}

TEST("tg transformed - t.transform(obj) and t(obj) mirror obj.transformed(t)")
{
    auto const t = tg::rigid_transform3f::make_rotation(tg::quat_f::make_rotation_z(tg::angle_f::make_from_degree(90)))
                       .composed(tg::rigid_transform3f::make_translation(tg::vec3f(1, 0, 0)));

    SECTION("both route straight back to the object, so all three spellings agree")
    {
        auto const s = tg::sphere3f(tg::pos3f(2, 0, 0), 3.0f);
        CHECK(t.transform(s) == s.transformed(t));
        CHECK(t(s) == s.transformed(t));

        auto const tri = tg::triangle3f(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0));
        CHECK(t.transform(tri) == tri.transformed(t));
        CHECK(t(tri) == tri.transformed(t));
    }

    SECTION("including the return type that changes")
    {
        auto const s = tg::sphere3f(tg::pos3f(0, 0, 0), 2.0f);
        auto const a = tg::affine_transform3f(t);
        static_assert(std::is_same_v<decltype(a.transform(s)), tg::ellipsoid3f>);
        static_assert(std::is_same_v<decltype(a(s)), tg::ellipsoid3f>);
        CHECK(a.transform(s) == s.transformed(a));
        CHECK(a(s) == s.transformed(a));
    }
}
