#pragma once

#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/impl/sampling.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Capsule: every point within `radius` of a segment, the segment's swept ball.
///
/// Represents {x : distance(x, axis) <= radius}, interior included, so intrinsic_dim is D.
/// In 2D it is a stadium.
/// tg::capsule_boundary shares the encoding and denotes the surface.
///
///     auto const c = tg::capsule3f(tg::segment3f(tg::pos3f(0, 0, 0), tg::pos3f(0, 2, 0)), 0.5f);
template <int D, class T>
struct tg::capsule
{
    static_assert(D > 0, "capsule requires a positive dimension");

    segment<D, T> axis;
    T radius = {};

    // construction
public:
    capsule() = default;

    explicit constexpr capsule(segment<D, T> const& axis, T radius) : axis(axis), radius(radius) {}

    // readings
public:
    [[nodiscard]] constexpr capsule_boundary<D, T> boundary() const { return capsule_boundary<D, T>(axis, radius); }

    // transformation
public:
    /// A similarity keeps it a capsule; anything that stretches the radius differently per direction does not.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else if constexpr (requires { tg::signed_similarity_transform<D, T>(t); })
        {
            auto const s = tg::signed_similarity_transform<D, T>(t);
            auto const scale = s.uniform_scale();
            return capsule(axis.transformed(s), radius * (scale < T(0) ? -scale : scale));
        }
        else
            static_assert(false,
                          "tg: a capsule only survives a similarity; under a wider map its round ends stop being "
                          "round, which no tg type holds.");
    }

    // measures and readings
public:
    [[nodiscard]] constexpr pos<D, T> centroid() const { return axis.centroid(); }
    [[nodiscard]] constexpr aabb<D, T> bounds() const
    {
        auto const b = axis.bounds();
        return aabb<D, T>(b.min - vec<D, T>(radius), b.max + vec<D, T>(radius));
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return axis.pos0; }
    /// in 2D the stadium's area; in 3D the area of its surface.
    [[nodiscard]] constexpr T area() const
        requires((D == 2 || D == 3) && tg::traits::has_sqrt<T>)
    {
        auto const h = axis.length();
        if constexpr (D == 2)
            return T(2) * radius * h + tg::pi<T> * radius * radius;
        else
            return T(2) * tg::pi<T> * radius * h + T(4) * tg::pi<T> * radius * radius;
    }
    [[nodiscard]] constexpr T perimeter() const
        requires(D == 2 && tg::traits::has_sqrt<T>)
    {
        return T(2) * axis.length() + T(2) * tg::pi<T> * radius;
    }
    [[nodiscard]] constexpr T volume() const
        requires(D == 3 && tg::traits::has_sqrt<T>)
    {
        return tg::pi<T> * radius * radius * (axis.length() + T(4) / T(3) * radius);
    }

    // sampling
public:
    /// by rejection from the bounds, the way the ball samples.
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto const b = this->bounds();
        while (true)
        {
            auto const p = b.sample_uniform(rng);
            auto const t = axis.parameter_of(p);
            if ((p - axis.at(t)).length_sqr() <= radius * radius)
                return p;
        }
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
    [[nodiscard]] friend constexpr bool operator==(capsule const&, capsule const&) = default;
};

/// The boundary of a tg::capsule: the points exactly `radius` from its axis, so intrinsic_dim is D - 1.
template <int D, class T>
struct tg::capsule_boundary
{
    static_assert(D > 0, "capsule_boundary requires a positive dimension");

    segment<D, T> axis;
    T radius = {};

    // construction
public:
    capsule_boundary() = default;

    explicit constexpr capsule_boundary(segment<D, T> const& axis, T radius) : axis(axis), radius(radius) {}

    // readings
public:
    [[nodiscard]] constexpr capsule<D, T> solid() const { return capsule<D, T>(axis, radius); }

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
    [[nodiscard]] constexpr aabb<D, T> bounds() const { return this->solid().bounds(); }
    [[nodiscard]] constexpr T length() const
        requires(D == 2 && tg::traits::has_sqrt<T>)
    {
        return this->solid().perimeter();
    }
    [[nodiscard]] constexpr T area() const
        requires(D == 3 && tg::traits::has_sqrt<T>)
    {
        return this->solid().area();
    }

    // sampling
public:
    /// the tube with probability of its share of the area, otherwise a ball's surface point, kept on the end it
    /// faces: the two hemispherical caps together are exactly one sphere.
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T> && D == 3)
    {
        auto const d = axis.pos1 - axis.pos0;
        auto const h = d.length();
        auto const u = d / h;
        auto const tube = T(2) * tg::pi<T> * radius * h;
        auto const caps = T(4) * tg::pi<T> * radius * radius;
        if (rng.uniform(T(0), tube + caps) < tube)
        {
            auto const [e0, e1] = tg::orthonormal_basis(u);
            auto const c = tg::impl::uniform_direction<2, T>(rng);
            return axis.at(rng.uniform(T(0), T(1))) + (e0 * c.data[0] + e1 * c.data[1]) * radius;
        }
        auto const dir = tg::impl::uniform_direction<3, T>(rng);
        return (tg::dot(dir, u) < T(0) ? axis.pos0 : axis.pos1) + dir * radius;
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
    [[nodiscard]] friend constexpr bool operator==(capsule_boundary const&, capsule_boundary const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::capsule<D, T>>
{
    static constexpr int intrinsic_dim = D;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

template <int D, class T>
struct tg::object_traits<tg::capsule_boundary<D, T>>
{
    static constexpr int intrinsic_dim = D - 1;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
