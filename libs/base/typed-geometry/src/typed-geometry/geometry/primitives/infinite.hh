#pragma once

#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/primitives/line.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/angle.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Infinite cylinder: every point within `radius` of a line.
///
/// Represents {x : distance(x, axis) <= radius}, unbounded along the axis; in 2D it is the slab between two lines.
/// tg::inf_cylinder_boundary is the surface alone.
template <int D, class T>
struct tg::inf_cylinder
{
    static_assert(D > 1, "an infinite cylinder needs at least two dimensions");

    line<D, T> axis;
    T radius = {};

    // construction
public:
    inf_cylinder() = default;

    explicit constexpr inf_cylinder(line<D, T> const& axis, T radius) : axis(axis), radius(radius) {}

    // readings
public:
    [[nodiscard]] constexpr inf_cylinder_boundary<D, T> boundary() const
    {
        return inf_cylinder_boundary<D, T>(axis, radius);
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return axis.origin; }

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
            return inf_cylinder(axis.transformed(s), radius * (scale < T(0) ? -scale : scale));
        }
        else
            static_assert(false, "tg: an infinite cylinder only survives a similarity; its affine image is elliptic.");
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
    [[nodiscard]] friend constexpr bool operator==(inf_cylinder const&, inf_cylinder const&) = default;
};

/// The surface of a tg::inf_cylinder: the points exactly `radius` from the axis, intrinsic_dim D - 1.
template <int D, class T>
struct tg::inf_cylinder_boundary
{
    static_assert(D > 1, "an infinite cylinder needs at least two dimensions");

    line<D, T> axis;
    T radius = {};

    // construction
public:
    inf_cylinder_boundary() = default;

    explicit constexpr inf_cylinder_boundary(line<D, T> const& axis, T radius) : axis(axis), radius(radius) {}

    // readings
public:
    [[nodiscard]] constexpr inf_cylinder<D, T> solid() const { return inf_cylinder<D, T>(axis, radius); }

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
    [[nodiscard]] friend constexpr bool operator==(inf_cylinder_boundary const&, inf_cylinder_boundary const&) = default;
};

/// Infinite cone: every ray from an apex within half the opening angle of a direction.
///
/// Represents {apex + v : angle(v, dir) <= opening_angle / 2}, the single nappe the direction points into.
/// `dir` is expected to be unit length, and the opening angle below 180 degrees, so the set is convex.
/// tg::inf_cone_boundary is the surface alone.
template <int D, class T>
struct tg::inf_cone
{
    static_assert(D > 1, "an infinite cone needs at least two dimensions");

    pos<D, T> apex;
    vec<D, T> dir;
    angle<T> opening_angle;

    // construction
public:
    inf_cone() = default;

    explicit constexpr inf_cone(pos<D, T> const& apex, vec<D, T> const& dir, angle<T> opening_angle)
      : apex(apex), dir(dir), opening_angle(opening_angle)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr inf_cone_boundary<D, T> boundary() const
    {
        return inf_cone_boundary<D, T>(apex, dir, opening_angle);
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return apex; }

    // transformation
public:
    /// A similarity preserves angles, so the opening stays; a reflection flips the direction with everything else.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else if constexpr (requires { tg::signed_similarity_transform<D, T>(t); })
        {
            auto const s = tg::signed_similarity_transform<D, T>(t);
            return inf_cone(apex.transformed(s), dir.transformed(s) / s.uniform_scale(), opening_angle);
        }
        else
            static_assert(false, "tg: an infinite cone only survives a similarity; its affine image is elliptic.");
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
    [[nodiscard]] friend constexpr bool operator==(inf_cone const&, inf_cone const&) = default;
};

/// The surface of a tg::inf_cone: the rays from the apex at exactly half the opening angle, intrinsic_dim D - 1.
template <int D, class T>
struct tg::inf_cone_boundary
{
    static_assert(D > 1, "an infinite cone needs at least two dimensions");

    pos<D, T> apex;
    vec<D, T> dir;
    angle<T> opening_angle;

    // construction
public:
    inf_cone_boundary() = default;

    explicit constexpr inf_cone_boundary(pos<D, T> const& apex, vec<D, T> const& dir, angle<T> opening_angle)
      : apex(apex), dir(dir), opening_angle(opening_angle)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr inf_cone<D, T> solid() const { return inf_cone<D, T>(apex, dir, opening_angle); }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return apex; }

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
    [[nodiscard]] friend constexpr bool operator==(inf_cone_boundary const&, inf_cone_boundary const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::inf_cylinder<D, T>>
{
    static constexpr int intrinsic_dim = D;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = false;
};

template <int D, class T>
struct tg::object_traits<tg::inf_cylinder_boundary<D, T>>
{
    static constexpr int intrinsic_dim = D - 1;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = false;
};

template <int D, class T>
struct tg::object_traits<tg::inf_cone<D, T>>
{
    static constexpr int intrinsic_dim = D;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = false;
};

template <int D, class T>
struct tg::object_traits<tg::inf_cone_boundary<D, T>>
{
    static constexpr int intrinsic_dim = D - 1;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = false;
};
