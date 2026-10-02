#pragma once

#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/impl/sampling.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/cylinder.hh>
#include <typed-geometry/geometry/primitives/sphere.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace tg::impl
{
/// bounds of the half ball on the normal's side: the base disk, the pole, and a full radius on every axis the
/// normal leans towards.
template <class T>
[[nodiscard]] constexpr aabb<3, T> hemisphere_bounds(pos<3, T> const& c, T r, vec<3, T> const& n)
{
    auto const e = tg::impl::disk_reach(n, r);
    auto b = tg::impl::bounds_of(c - e, c + e, c + n * r);
    for (int k = 0; k < 3; ++k)
    {
        if (n.data[k] > T(0))
            b.max.data[k] = c.data[k] + r;
        if (n.data[k] < T(0))
            b.min.data[k] = c.data[k] - r;
    }
    return b;
}
} // namespace tg::impl

/// Hemisphere: the half of a ball on the side its unit normal points to, the flat base included.
///
/// Represents {x : distance(x, center) <= radius, dot(normal, x - center) >= 0}.
/// 3D only; tg::hemisphere_boundary is the dome and the base, tg::hemisphere_mantle the dome alone.
template <int D, class T>
struct tg::hemisphere
{
    static_assert(D == 3, "a hemisphere is 3D");

    pos<D, T> center;
    T radius = {};
    vec<D, T> normal;

    // construction
public:
    hemisphere() = default;

    explicit constexpr hemisphere(pos<D, T> const& center, T radius, vec<D, T> const& normal)
      : center(center), radius(radius), normal(normal)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr hemisphere_boundary<D, T> boundary() const
    {
        return hemisphere_boundary<D, T>(center, radius, normal);
    }
    [[nodiscard]] constexpr hemisphere_mantle<D, T> mantle() const
    {
        return hemisphere_mantle<D, T>(center, radius, normal);
    }
    /// the base disk.
    [[nodiscard]] constexpr cc::fixed_array<sphere<2, 3, T>, 1> caps() const
    {
        return {{sphere<2, 3, T>(center, radius, normal)}};
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
            auto const abs_scale = scale < T(0) ? -scale : scale;
            // the image of the normal points into the image of the half it bounded, so only the magnitude divides out
            return hemisphere(center.transformed(s), radius * abs_scale, normal.transformed(s) / abs_scale);
        }
        else
            static_assert(false, "tg: a hemisphere only survives a similarity; under a wider map it is half an "
                                 "ellipsoid, which tg has no type for.");
    }

    // measures and readings
public:
    /// 3/8 of the radius above the base.
    [[nodiscard]] constexpr pos<D, T> centroid() const { return center + normal * (radius * T(3) / T(8)); }
    [[nodiscard]] constexpr aabb<D, T> bounds() const
        requires(tg::traits::has_sqrt<T>)
    {
        return tg::impl::hemisphere_bounds(center, radius, normal);
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return center; }
    /// the whole surface: the dome and the base.
    [[nodiscard]] constexpr T area() const { return T(3) * tg::pi<T> * radius * radius; }
    [[nodiscard]] constexpr T volume() const { return T(2) / T(3) * tg::pi<T> * radius * radius * radius; }

    // sampling
public:
    /// a ball sample, mirrored into the half the normal points to.
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto v = tg::impl::uniform_in_unit_ball<3, T>(rng);
        auto const up = tg::dot(v, normal);
        if (up < T(0))
            v = v - normal * (T(2) * up);
        return center + v * radius;
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
    [[nodiscard]] constexpr bool may_intersect(Obj const& obj) const;
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
    [[nodiscard]] friend constexpr bool operator==(hemisphere const&, hemisphere const&) = default;
};

/// The whole surface of a tg::hemisphere: the dome and the base disk, intrinsic_dim 2.
template <int D, class T>
struct tg::hemisphere_boundary
{
    static_assert(D == 3, "a hemisphere is 3D");

    pos<D, T> center;
    T radius = {};
    vec<D, T> normal;

    // construction
public:
    hemisphere_boundary() = default;

    explicit constexpr hemisphere_boundary(pos<D, T> const& center, T radius, vec<D, T> const& normal)
      : center(center), radius(radius), normal(normal)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr hemisphere<D, T> solid() const { return hemisphere<D, T>(center, radius, normal); }
    [[nodiscard]] constexpr hemisphere_mantle<D, T> mantle() const
    {
        return hemisphere_mantle<D, T>(center, radius, normal);
    }

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
    /// the dome's centroid is half a radius up and carries two thirds of the area; the base's is the center.
    [[nodiscard]] constexpr pos<D, T> centroid() const { return center + normal * (radius / T(3)); }
    [[nodiscard]] constexpr aabb<D, T> bounds() const
        requires(tg::traits::has_sqrt<T>)
    {
        return tg::impl::hemisphere_bounds(center, radius, normal);
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return center; }
    [[nodiscard]] constexpr T area() const { return T(3) * tg::pi<T> * radius * radius; }

    // sampling
public:
    /// the base with a third of the probability, the dome with the rest.
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        if (rng.uniform(T(0), T(3)) < T(1))
            return tg::impl::sample_disk(rng, center, normal, radius);
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
    [[nodiscard]] constexpr bool may_intersect(Obj const& obj) const;
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
    [[nodiscard]] friend constexpr bool operator==(hemisphere_boundary const&, hemisphere_boundary const&) = default;
};

/// The dome of a hemisphere without its base, intrinsic_dim 2.
template <int D, class T>
struct tg::hemisphere_mantle
{
    static_assert(D == 3, "a hemisphere is 3D");

    pos<D, T> center;
    T radius = {};
    vec<D, T> normal;

    // construction
public:
    hemisphere_mantle() = default;

    explicit constexpr hemisphere_mantle(pos<D, T> const& center, T radius, vec<D, T> const& normal)
      : center(center), radius(radius), normal(normal)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr hemisphere<D, T> solid() const { return hemisphere<D, T>(center, radius, normal); }

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
    [[nodiscard]] constexpr pos<D, T> centroid() const { return center + normal * (radius / T(2)); }
    [[nodiscard]] constexpr aabb<D, T> bounds() const
        requires(tg::traits::has_sqrt<T>)
    {
        return tg::impl::hemisphere_bounds(center, radius, normal);
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return center + normal * radius; }
    [[nodiscard]] constexpr T area() const { return T(2) * tg::pi<T> * radius * radius; }

    // sampling
public:
    /// a sphere sample, mirrored onto the dome.
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto v = tg::impl::uniform_direction<3, T>(rng);
        auto const up = tg::dot(v, normal);
        if (up < T(0))
            v = v - normal * (T(2) * up);
        return center + v * radius;
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
    [[nodiscard]] constexpr bool may_intersect(Obj const& obj) const;
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
    [[nodiscard]] friend constexpr bool operator==(hemisphere_mantle const&, hemisphere_mantle const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::hemisphere<D, T>>
{
    static constexpr int intrinsic_dim = 3;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

template <int D, class T>
struct tg::object_traits<tg::hemisphere_boundary<D, T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

template <int D, class T>
struct tg::object_traits<tg::hemisphere_mantle<D, T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
