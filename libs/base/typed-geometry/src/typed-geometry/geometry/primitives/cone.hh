#pragma once

#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/impl/sampling.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/cylinder.hh>
#include <typed-geometry/geometry/primitives/infinite.hh>
#include <typed-geometry/geometry/primitives/sphere.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Cone: an apex, and a disk of `radius` at the far end of `axis` from it, perpendicular to it.
///
/// Represents the solid {apex + axis * s + radial : s in [0, 1], radial perpendicular to axis, |radial| <= radius * s}.
/// `axis` runs from the apex to the base center, so its length is the height.
/// 3D only; tg::cone_boundary is the whole surface, tg::cone_mantle the slanted part alone.
///
///     auto const spot = tg::cone3f(tg::pos3f(0, 5, 0), tg::vec3f(0, -5, 0), 2.0f);   // a light cone pointing down
template <int D, class T>
struct tg::cone
{
    static_assert(D == 3, "a cone is 3D");

    pos<D, T> apex;
    vec<D, T> axis;
    T radius = {};

    // construction
public:
    cone() = default;

    explicit constexpr cone(pos<D, T> const& apex, vec<D, T> const& axis, T radius)
      : apex(apex), axis(axis), radius(radius)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr cone_boundary<D, T> boundary() const { return cone_boundary<D, T>(apex, axis, radius); }
    [[nodiscard]] constexpr cone_mantle<D, T> mantle() const { return cone_mantle<D, T>(apex, axis, radius); }
    [[nodiscard]] constexpr pos<D, T> base_center() const { return apex + axis; }
    /// the infinite cone the slant extends to: opening angle twice atan(radius / height).
    [[nodiscard]] constexpr inf_cone<D, T> unbounded() const
        requires(tg::traits::has_sqrt<T> && tg::traits::has_trigonometry<T>)
    {
        return inf_cone<D, T>(apex, axis.normalized(), tg::atan(radius / axis.length()) * T(2));
    }
    /// the base disk, facing away from the apex.
    [[nodiscard]] constexpr cc::fixed_array<sphere<2, 3, T>, 1> caps() const
        requires(tg::traits::has_sqrt<T>)
    {
        return {{sphere<2, 3, T>(apex + axis, radius, axis.normalized())}};
    }

    // transformation
public:
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else if constexpr (requires { tg::signed_similarity_transform<D, T>(t); })
        {
            auto const s = tg::signed_similarity_transform<D, T>(t);
            auto const scale = s.uniform_scale();
            return cone(apex.transformed(s), axis.transformed(s), radius * (scale < T(0) ? -scale : scale));
        }
        else
            static_assert(false, "tg: a cone only survives a similarity; its affine image is an elliptic cone, which "
                                 "tg has no type for.");
    }

    // measures and readings
public:
    /// a quarter of the way from the base to the apex.
    [[nodiscard]] constexpr pos<D, T> centroid() const { return apex + axis * (T(3) / T(4)); }
    [[nodiscard]] constexpr aabb<D, T> bounds() const
        requires(tg::traits::has_sqrt<T>)
    {
        auto const e = tg::impl::disk_reach(axis, radius);
        auto const base = tg::impl::bounds_of(apex + axis - e, apex + axis + e);
        return tg::impl::bounds_of(apex, base.min, base.max);
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return apex; }
    /// the whole surface: base and mantle.
    [[nodiscard]] constexpr T area() const
        requires(tg::traits::has_sqrt<T>)
    {
        auto const h = axis.length();
        return tg::pi<T> * radius * (radius + tg::sqrt(h * h + radius * radius));
    }
    [[nodiscard]] constexpr T volume() const
        requires(tg::traits::has_sqrt<T>)
    {
        return tg::pi<T> * radius * radius * axis.length() / T(3);
    }

    // sampling
public:
    /// the cross-section's area grows with the square of the distance from the apex, so that distance is h * cbrt(u).
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T> && tg::traits::has_exponential<T>)
    {
        auto const s = tg::pow(rng.uniform(T(0), T(1)), T(1) / T(3));
        return tg::impl::sample_disk(rng, apex + axis * s, axis.normalized(), radius * s);
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
    [[nodiscard]] constexpr bool intersects(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr bool contains(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(cone const&, cone const&) = default;
};

/// The whole surface of a tg::cone: its base disk and its mantle, intrinsic_dim 2.
template <int D, class T>
struct tg::cone_boundary
{
    static_assert(D == 3, "a cone is 3D");

    pos<D, T> apex;
    vec<D, T> axis;
    T radius = {};

    // construction
public:
    cone_boundary() = default;

    explicit constexpr cone_boundary(pos<D, T> const& apex, vec<D, T> const& axis, T radius)
      : apex(apex), axis(axis), radius(radius)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr cone<D, T> solid() const { return cone<D, T>(apex, axis, radius); }
    [[nodiscard]] constexpr cone_mantle<D, T> mantle() const { return cone_mantle<D, T>(apex, axis, radius); }

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
    [[nodiscard]] constexpr aabb<D, T> bounds() const
        requires(tg::traits::has_sqrt<T>)
    {
        return this->solid().bounds();
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return apex; }
    [[nodiscard]] constexpr T area() const
        requires(tg::traits::has_sqrt<T>)
    {
        return this->solid().area();
    }

    // sampling
public:
    /// the base or the mantle, each by its share of the area.
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto const base = tg::pi<T> * radius * radius;
        if (rng.uniform(T(0), this->area()) < base)
            return tg::impl::sample_disk(rng, apex + axis, axis.normalized(), radius);
        return this->mantle().sample_uniform(rng);
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
    [[nodiscard]] constexpr bool intersects(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr bool contains(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(cone_boundary const&, cone_boundary const&) = default;
};

/// The slanted surface of a cone without its base, intrinsic_dim 2.
template <int D, class T>
struct tg::cone_mantle
{
    static_assert(D == 3, "a cone is 3D");

    pos<D, T> apex;
    vec<D, T> axis;
    T radius = {};

    // construction
public:
    cone_mantle() = default;

    explicit constexpr cone_mantle(pos<D, T> const& apex, vec<D, T> const& axis, T radius)
      : apex(apex), axis(axis), radius(radius)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr cone<D, T> solid() const { return cone<D, T>(apex, axis, radius); }

    // transformation
public:
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else
            return this->solid().transformed(t).mantle();
    }

    // measures and readings
public:
    [[nodiscard]] constexpr aabb<D, T> bounds() const
        requires(tg::traits::has_sqrt<T>)
    {
        return this->solid().bounds();
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return apex; }
    [[nodiscard]] constexpr T area() const
        requires(tg::traits::has_sqrt<T>)
    {
        auto const h = axis.length();
        return tg::pi<T> * radius * tg::sqrt(h * h + radius * radius);
    }

    // sampling
public:
    /// the circumference grows linearly from the apex, so the distance from it is h * sqrt(u).
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto const s = tg::sqrt(rng.uniform(T(0), T(1)));
        auto const [e0, e1] = tg::orthonormal_basis(axis.normalized());
        auto const c = tg::impl::uniform_direction<2, T>(rng);
        return apex + axis * s + (e0 * c.data[0] + e1 * c.data[1]) * (radius * s);
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
    [[nodiscard]] constexpr bool intersects(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr bool contains(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(cone_mantle const&, cone_mantle const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::cone<D, T>>
{
    static constexpr int intrinsic_dim = 3;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

template <int D, class T>
struct tg::object_traits<tg::cone_boundary<D, T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

template <int D, class T>
struct tg::object_traits<tg::cone_mantle<D, T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
