#pragma once

#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/impl/sampling.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/ellipsoid.hh>
#include <typed-geometry/geometry/primitives/plane.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace tg
{
/// Solid sphere — a ball — stored as a center and a radius.
///
/// Represents {x : distance(x, center) <= radius} inside the flat it lives in, interior included, so intrinsic_dim is D.
/// D is the dimension of that flat, DAmbient the dimension of the space the flat sits in.
/// `sphere<2, 2, T>` is a disk in the plane, `sphere<3, 3, T>` the ordinary ball, `sphere<2, 3, T>` that same disk lying in 3D.
/// The radius should not be negative.
///
/// The primary template is left **undefined**: what a sphere has to store depends on the pair, so every supported pair is its own specialization.
/// A sphere that spans its ambient space is {center, radius}; one that does not needs the flat named as well, which {center, radius} alone does not do.
/// Asking for a pair that has no specialization — a disk in 4D, say — is an incomplete type, not a silently wrong encoding.
///
/// tg::sphere_boundary shares this encoding and denotes the surface; `.boundary()` and `.solid()` convert between them.
template <int D, int DAmbient, class T>
struct sphere;

/// The boundary of a tg::sphere: {x : distance(x, center) == radius} inside its flat, so intrinsic_dim is D - 1.
/// `sphere_boundary<3, 3, T>` is the sphere surface, `sphere_boundary<2, 2, T>` a circle, `sphere_boundary<2, 3, T>` a circle lying in 3D.
/// Same specializations and storage as tg::sphere.
template <int D, int DAmbient, class T>
struct sphere_boundary;

} // namespace tg

/// A ball spanning its ambient space: an ordinary ball in 3D, a disk in 2D.
///
///     auto const s = tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f);
///     auto const e = s.transformed(some_affine);   // an ellipsoid, not a sphere
///     auto const surface = s.boundary();          // a tg::sphere3f_surface
template <int D, class T>
struct tg::sphere<D, D, T>
{
    static_assert(D > 0, "sphere requires a positive dimension");

    pos<D, T> center;
    T radius = {};

    // construction
public:
    sphere() = default;

    explicit constexpr sphere(pos<D, T> const& center, T radius) : center(center), radius(radius) {}

    // readings
public:
    [[nodiscard]] constexpr sphere_boundary<D, D, T> boundary() const
    {
        return sphere_boundary<D, D, T>(center, radius);
    }

    // transformation
public:
    /// A similarity preserves angles, so a sphere stays a sphere; anything wider turns it into an ellipsoid.
    ///
    /// A signed uniform scale is still sphere-preserving — in 3D it makes the similarity the FULL conformal group,
    /// since a negative scale composed with a half-turn is a plane reflection.
    /// Only that class pays for taking the magnitude; with positive factors the radius is just multiplied.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::similarity_transform<D, T>(t); })
        {
            auto const s = tg::similarity_transform<D, T>(t);
            return sphere(center.transformed(s), radius * s.uniform_scale());
        }
        else if constexpr (requires { tg::signed_similarity_transform<D, T>(t); })
        {
            auto const s = tg::signed_similarity_transform<D, T>(t);
            T const scale = s.uniform_scale();
            return sphere(center.transformed(s), radius * (scale < T(0) ? -scale : scale));
        }
        else if constexpr (requires { tg::affine_transform<D, T>(t); })
        {
            auto const a = tg::affine_transform<D, T>(t);
            auto const m = a.linear_mat();

            // the semi-axes are the images of the radius vectors along each axis
            vec<D, T> axes[D] = {};
            for (int i = 0; i < D; ++i)
                axes[i] = m.cols[i] * radius;

            return ellipsoid<D, D, T>(center.transformed(a), axes);
        }
        else
            static_assert(false, "tg: a sphere only survives a similarity (-> sphere) or an affine map "
                                 "(-> ellipsoid). Under a projective map it becomes a general quadric, which tg has no "
                                 "type for yet.");
    }

    // measures and readings
public:
    [[nodiscard]] constexpr pos<D, T> centroid() const { return center; }
    [[nodiscard]] constexpr aabb<D, T> bounds() const
    {
        return aabb<D, T>(center - vec<D, T>(radius), center + vec<D, T>(radius));
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return center; }
    /// in 2D the disk's area; in 3D the area of the surface.
    [[nodiscard]] constexpr T area() const
        requires(D == 2 || D == 3)
    {
        if constexpr (D == 2)
            return tg::pi<T> * radius * radius;
        else
            return T(4) * tg::pi<T> * radius * radius;
    }
    [[nodiscard]] constexpr T perimeter() const
        requires(D == 2)
    {
        return T(2) * tg::pi<T> * radius;
    }
    [[nodiscard]] constexpr T volume() const
        requires(D == 3)
    {
        return T(4) / T(3) * tg::pi<T> * radius * radius * radius;
    }

    // sampling
public:
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T> && (D == 2 || D == 3))
    {
        return center + tg::impl::uniform_in_unit_ball<D, T>(rng) * radius;
    }

    // queries: defined per verb in geometry/query/, see libs/base/typed-geometry/docs/plans/geometry-query-matrix.md
public:
    template <class Obj>
    [[nodiscard]] constexpr auto project_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_points_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_point_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_sqr_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto signed_distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto contains(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersects(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(sphere const&, sphere const&) = default;
};

/// A disk lying in 3D, which is a plane's worth of disk plus the plane.
///
/// {center, radius} says nothing about which plane the disk lies in, so this case carries the plane's normal on top.
/// The normal is expected to be unit-length; its sign is a convention, both orientations name the same disk.
///
///     auto const d = tg::disk3f(tg::pos3f(0, 0, 0), 1.0f, tg::vec3f(0, 0, 1));   // unit disk in the xy-plane
template <class T>
struct tg::sphere<2, 3, T>
{
    pos<3, T> center;
    T radius = {};
    vec<3, T> normal;

    // construction
public:
    sphere() = default;

    explicit constexpr sphere(pos<3, T> const& center, T radius, vec<3, T> const& normal)
      : center(center), radius(radius), normal(normal)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr sphere_boundary<2, 3, T> boundary() const
    {
        return sphere_boundary<2, 3, T>(center, radius, normal);
    }

    // transformation
public:
    /// A similarity carries the plane along with the circle, and the linear part is its uniform scale times a rotation —
    /// so dividing the image of a unit normal by that scale re-normalizes it exactly, with no sqrt.
    /// A signed scale flips the normal, which names the same plane.
    ///
    /// The affine image is an ellipse in space — an ellipsoid<2, 3, T> — but naming its semi-axes needs an orthonormal
    /// basis of the disk's plane, which linalg has no routine for yet, so that pair is a compile error for now.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::similarity_transform<3, T>(t); })
        {
            auto const s = tg::similarity_transform<3, T>(t);
            T const scale = s.uniform_scale();
            return sphere(center.transformed(s), radius * scale, normal.transformed(s) / scale);
        }
        else if constexpr (requires { tg::signed_similarity_transform<3, T>(t); })
        {
            auto const s = tg::signed_similarity_transform<3, T>(t);
            T const scale = s.uniform_scale();
            return sphere(center.transformed(s), radius * (scale < T(0) ? -scale : scale), normal.transformed(s) / scale);
        }
        else
            static_assert(false, "tg: an embedded sphere only survives a similarity. Its affine image is an ellipse in "
                                 "space, which needs an orthonormal basis of its plane — a linalg routine tg does not "
                                 "have yet.");
    }

    // measures and readings
public:
    [[nodiscard]] constexpr pos<3, T> centroid() const { return center; }
    /// along each axis the disk reaches radius * sqrt(1 - normal_k^2): its rim tilted out of that axis.
    [[nodiscard]] constexpr aabb<3, T> bounds() const
        requires(tg::traits::has_sqrt<T>)
    {
        auto e = vec<3, T>();
        for (int k = 0; k < 3; ++k)
            e.data[k] = radius * tg::sqrt(T(1) - normal.data[k] * normal.data[k]);
        return aabb<3, T>(center - e, center + e);
    }
    [[nodiscard]] constexpr tg::plane<3, T> plane() const
    {
        return tg::plane<3, T>(normal, tg::dot(normal, center - pos<3, T>()));
    }
    [[nodiscard]] constexpr pos<3, T> any_point() const { return center; }
    [[nodiscard]] constexpr T area() const { return tg::pi<T> * radius * radius; }
    [[nodiscard]] constexpr T perimeter() const { return T(2) * tg::pi<T> * radius; }

    // queries: defined per verb in geometry/query/, see libs/base/typed-geometry/docs/plans/geometry-query-matrix.md
public:
    template <class Obj>
    [[nodiscard]] constexpr auto project_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_points_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_point_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_sqr_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto signed_distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto contains(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersects(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(sphere const&, sphere const&) = default;
};

/// The surface of a ball spanning its ambient space: the sphere surface in 3D, a circle in 2D.
template <int D, class T>
struct tg::sphere_boundary<D, D, T>
{
    static_assert(D > 0, "sphere_boundary requires a positive dimension");

    pos<D, T> center;
    T radius = {};

    // construction
public:
    sphere_boundary() = default;

    explicit constexpr sphere_boundary(pos<D, T> const& center, T radius) : center(center), radius(radius) {}

    // readings
public:
    [[nodiscard]] constexpr sphere<D, D, T> solid() const { return sphere<D, D, T>(center, radius); }

    // transformation
public:
    /// Whatever the solid becomes, this is the boundary of that: a sphere surface, or an ellipsoid surface.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else
            return this->solid().transformed(t).boundary();
    }

    // measures and readings
public:
    [[nodiscard]] constexpr pos<D, T> centroid() const { return center; }
    [[nodiscard]] constexpr aabb<D, T> bounds() const
    {
        return aabb<D, T>(center - vec<D, T>(radius), center + vec<D, T>(radius));
    }
    /// the point of the surface along the first axis.
    [[nodiscard]] constexpr pos<D, T> any_point() const
    {
        auto p = center;
        p.data[0] = p.data[0] + radius;
        return p;
    }
    /// the circumference of a circle.
    [[nodiscard]] constexpr T length() const
        requires(D == 2)
    {
        return T(2) * tg::pi<T> * radius;
    }
    [[nodiscard]] constexpr T area() const
        requires(D == 3)
    {
        return T(4) * tg::pi<T> * radius * radius;
    }

    // sampling
public:
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T> && (D == 2 || D == 3))
    {
        return center + tg::impl::uniform_direction<D, T>(rng) * radius;
    }

    // queries: defined per verb in geometry/query/, see libs/base/typed-geometry/docs/plans/geometry-query-matrix.md
public:
    template <class Obj>
    [[nodiscard]] constexpr auto project_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_points_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_point_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_sqr_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto signed_distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto contains(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersects(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(sphere_boundary const&, sphere_boundary const&) = default;
};

/// A circle lying in 3D: the boundary of a tg::disk3, with the same {center, radius, normal}.
template <class T>
struct tg::sphere_boundary<2, 3, T>
{
    pos<3, T> center;
    T radius = {};
    vec<3, T> normal;

    // construction
public:
    sphere_boundary() = default;

    explicit constexpr sphere_boundary(pos<3, T> const& center, T radius, vec<3, T> const& normal)
      : center(center), radius(radius), normal(normal)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr sphere<2, 3, T> solid() const { return sphere<2, 3, T>(center, radius, normal); }

    // transformation
public:
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else
            return this->solid().transformed(t).boundary();
    }

    // measures and readings
public:
    [[nodiscard]] constexpr pos<3, T> centroid() const { return center; }
    /// along each axis the disk reaches radius * sqrt(1 - normal_k^2): its rim tilted out of that axis.
    [[nodiscard]] constexpr aabb<3, T> bounds() const
        requires(tg::traits::has_sqrt<T>)
    {
        auto e = vec<3, T>();
        for (int k = 0; k < 3; ++k)
            e.data[k] = radius * tg::sqrt(T(1) - normal.data[k] * normal.data[k]);
        return aabb<3, T>(center - e, center + e);
    }
    [[nodiscard]] constexpr tg::plane<3, T> plane() const
    {
        return tg::plane<3, T>(normal, tg::dot(normal, center - pos<3, T>()));
    }
    [[nodiscard]] constexpr T length() const { return T(2) * tg::pi<T> * radius; }

    // queries: defined per verb in geometry/query/, see libs/base/typed-geometry/docs/plans/geometry-query-matrix.md
public:
    template <class Obj>
    [[nodiscard]] constexpr auto project_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_points_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_point_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_sqr_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto signed_distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto contains(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersects(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(sphere_boundary const&, sphere_boundary const&) = default;
};

template <int D, int DAmbient, class T>
struct tg::object_traits<tg::sphere<D, DAmbient, T>>
{
    static constexpr int intrinsic_dim = D;
    static constexpr int ambient_dim = DAmbient;
    static constexpr bool is_finite = true;
};

template <int D, int DAmbient, class T>
struct tg::object_traits<tg::sphere_boundary<D, DAmbient, T>>
{
    static constexpr int intrinsic_dim = D - 1;
    static constexpr int ambient_dim = DAmbient;
    static constexpr bool is_finite = true;
};
