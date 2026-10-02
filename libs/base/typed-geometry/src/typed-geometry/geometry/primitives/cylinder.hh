#pragma once

#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/impl/bounds_of.hh>
#include <typed-geometry/geometry/impl/sampling.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/infinite.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/primitives/sphere.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace tg::impl
{
/// a point uniform on the disk of radius r around c, perpendicular to the unit vector u.
template <class T>
[[nodiscard]] pos<3, T> sample_disk(cc::random& rng, pos<3, T> const& c, vec<3, T> const& u, T r)
{
    auto const [e0, e1] = tg::orthonormal_basis(u);
    auto const d = tg::impl::uniform_in_unit_ball<2, T>(rng);
    return c + (e0 * d.data[0] + e1 * d.data[1]) * r;
}
} // namespace tg::impl

/// Cylinder: a disk of `radius` swept along a segment, its caps flat and perpendicular to it.
///
/// Represents the solid {x : the foot of x on the axis line lies on the axis, and x is within radius of it}.
/// 3D only.
/// tg::cylinder_boundary is its whole surface, tg::cylinder_mantle the curved part alone — the tube.
///
///     auto const c = tg::cylinder3f(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 0, 2)), 0.5f);
template <int D, class T>
struct tg::cylinder
{
    static_assert(D == 3, "a cylinder is 3D");

    segment<D, T> axis;
    T radius = {};

    // construction
public:
    cylinder() = default;

    explicit constexpr cylinder(segment<D, T> const& axis, T radius) : axis(axis), radius(radius) {}

    // readings
public:
    [[nodiscard]] constexpr cylinder_boundary<D, T> boundary() const { return cylinder_boundary<D, T>(axis, radius); }
    [[nodiscard]] constexpr cylinder_mantle<D, T> mantle() const { return cylinder_mantle<D, T>(axis, radius); }
    /// the infinite cylinder around the axis's line.
    [[nodiscard]] constexpr inf_cylinder<D, T> unbounded() const
    {
        return inf_cylinder<D, T>(axis.unbounded(), radius);
    }
    /// the two end disks, at pos0 and at pos1, both facing along the axis.
    [[nodiscard]] constexpr cc::fixed_array<sphere<2, 3, T>, 2> caps() const
        requires(tg::traits::has_sqrt<T>)
    {
        auto const n = (axis.pos1 - axis.pos0).normalized();
        return {{sphere<2, 3, T>(axis.pos0, radius, n), sphere<2, 3, T>(axis.pos1, radius, n)}};
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
            return cylinder(axis.transformed(s), radius * (scale < T(0) ? -scale : scale));
        }
        else
            static_assert(false, "tg: a cylinder only survives a similarity; its affine image is an elliptic cylinder, "
                                 "which tg has no type for.");
    }

    // measures and readings
public:
    [[nodiscard]] constexpr pos<D, T> centroid() const { return axis.centroid(); }
    [[nodiscard]] constexpr aabb<D, T> bounds() const
        requires(tg::traits::has_sqrt<T>)
    {
        auto const e = tg::impl::disk_reach(axis.pos1 - axis.pos0, radius);
        auto const b = axis.bounds();
        return aabb<D, T>(b.min - e, b.max + e);
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return axis.pos0; }
    /// the area of the whole surface: the tube and both caps.
    [[nodiscard]] constexpr T area() const
        requires(tg::traits::has_sqrt<T>)
    {
        return T(2) * tg::pi<T> * radius * (axis.length() + radius);
    }
    [[nodiscard]] constexpr T volume() const
        requires(tg::traits::has_sqrt<T>)
    {
        return tg::pi<T> * radius * radius * axis.length();
    }

    // sampling
public:
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto const u = (axis.pos1 - axis.pos0).normalized();
        return tg::impl::sample_disk(rng, axis.at(rng.uniform(T(0), T(1))), u, radius);
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
    [[nodiscard]] friend constexpr bool operator==(cylinder const&, cylinder const&) = default;
};

/// The whole surface of a tg::cylinder: the tube and both caps, so intrinsic_dim is 2.
template <int D, class T>
struct tg::cylinder_boundary
{
    static_assert(D == 3, "a cylinder is 3D");

    segment<D, T> axis;
    T radius = {};

    // construction
public:
    cylinder_boundary() = default;

    explicit constexpr cylinder_boundary(segment<D, T> const& axis, T radius) : axis(axis), radius(radius) {}

    // readings
public:
    [[nodiscard]] constexpr cylinder<D, T> solid() const { return cylinder<D, T>(axis, radius); }
    [[nodiscard]] constexpr cylinder_mantle<D, T> mantle() const { return cylinder_mantle<D, T>(axis, radius); }

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
    [[nodiscard]] constexpr pos<D, T> centroid() const { return axis.centroid(); }
    [[nodiscard]] constexpr aabb<D, T> bounds() const
        requires(tg::traits::has_sqrt<T>)
    {
        return this->solid().bounds();
    }
    [[nodiscard]] constexpr T area() const
        requires(tg::traits::has_sqrt<T>)
    {
        return this->solid().area();
    }

    // sampling
public:
    /// the tube or a cap, each by its share of the area.
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto const tube = T(2) * tg::pi<T> * radius * axis.length();
        auto const cap = tg::pi<T> * radius * radius;
        auto const pick = rng.uniform(T(0), tube + T(2) * cap);
        if (pick < tube)
            return this->mantle().sample_uniform(rng);
        auto const u = (axis.pos1 - axis.pos0).normalized();
        return tg::impl::sample_disk(rng, pick < tube + cap ? axis.pos0 : axis.pos1, u, radius);
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
    [[nodiscard]] friend constexpr bool operator==(cylinder_boundary const&, cylinder_boundary const&) = default;
};

/// The curved part of a cylinder's surface without its caps: a tube open at both ends, intrinsic_dim 2.
template <int D, class T>
struct tg::cylinder_mantle
{
    static_assert(D == 3, "a cylinder is 3D");

    segment<D, T> axis;
    T radius = {};

    // construction
public:
    cylinder_mantle() = default;

    explicit constexpr cylinder_mantle(segment<D, T> const& axis, T radius) : axis(axis), radius(radius) {}

    // readings
public:
    [[nodiscard]] constexpr cylinder<D, T> solid() const { return cylinder<D, T>(axis, radius); }

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
    [[nodiscard]] constexpr pos<D, T> centroid() const { return axis.centroid(); }
    [[nodiscard]] constexpr aabb<D, T> bounds() const
        requires(tg::traits::has_sqrt<T>)
    {
        return this->solid().bounds();
    }
    [[nodiscard]] constexpr T area() const
        requires(tg::traits::has_sqrt<T>)
    {
        return T(2) * tg::pi<T> * radius * axis.length();
    }

    // sampling
public:
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto const [e0, e1] = tg::orthonormal_basis((axis.pos1 - axis.pos0).normalized());
        auto const c = tg::impl::uniform_direction<2, T>(rng);
        return axis.at(rng.uniform(T(0), T(1))) + (e0 * c.data[0] + e1 * c.data[1]) * radius;
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
    [[nodiscard]] friend constexpr bool operator==(cylinder_mantle const&, cylinder_mantle const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::cylinder<D, T>>
{
    static constexpr int intrinsic_dim = 3;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

template <int D, class T>
struct tg::object_traits<tg::cylinder_boundary<D, T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

template <int D, class T>
struct tg::object_traits<tg::cylinder_mantle<D, T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
