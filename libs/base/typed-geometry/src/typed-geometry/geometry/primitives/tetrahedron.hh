#pragma once

#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/impl/bounds_of.hh>
#include <typed-geometry/geometry/impl/sampling.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/primitives/triangle.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/comp.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Tetrahedron: the solid convex hull of four points, interior included, intrinsic_dim 3.
///
/// The vertex order is free; nothing depends on the orientation.
/// tg::tetrahedron_boundary is the four faces.
template <int D, class T>
struct tg::tetrahedron
{
    static_assert(D == 3, "a tetrahedron is 3D");

    pos<D, T> pos0;
    pos<D, T> pos1;
    pos<D, T> pos2;
    pos<D, T> pos3;

    // construction
public:
    tetrahedron() = default;

    explicit constexpr tetrahedron(pos<D, T> const& p0, pos<D, T> const& p1, pos<D, T> const& p2, pos<D, T> const& p3)
      : pos0(p0), pos1(p1), pos2(p2), pos3(p3)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr tetrahedron_boundary<D, T> boundary() const
    {
        return tetrahedron_boundary<D, T>(pos0, pos1, pos2, pos3);
    }

    // transformation
public:
    /// Every vertex maps as a point; an affine map keeps the hull a tetrahedron.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else if constexpr (requires { tg::affine_transform<D, T>(t); })
        {
            auto const a = tg::affine_transform<D, T>(t);
            return tetrahedron(pos0.transformed(a), pos1.transformed(a), pos2.transformed(a), pos3.transformed(a));
        }
        else
            static_assert(false, "tg: a tetrahedron is transformed by an affine map here; a projective one would need "
                                 "its vertices kept in front of the projection.");
    }

    // measures and readings
public:
    [[nodiscard]] constexpr T volume() const
    {
        auto const d = tg::dot(tg::dual(tg::cross(pos1 - pos0, pos2 - pos0)), pos3 - pos0);
        return tg::abs(d) / T(6);
    }
    /// the area of the four faces.
    [[nodiscard]] constexpr T area() const
        requires(tg::traits::has_sqrt<T>)
    {
        auto const f = this->faces();
        return f[0].area() + f[1].area() + f[2].area() + f[3].area();
    }
    [[nodiscard]] constexpr pos<D, T> centroid() const
    {
        return pos0 + ((pos1 - pos0) + (pos2 - pos0) + (pos3 - pos0)) / T(4);
    }
    [[nodiscard]] constexpr aabb<D, T> bounds() const { return tg::impl::bounds_of(pos0, pos1, pos2, pos3); }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return pos0; }
    [[nodiscard]] constexpr cc::fixed_array<pos<D, T>, 4> vertices() const { return {{pos0, pos1, pos2, pos3}}; }
    [[nodiscard]] constexpr cc::fixed_array<segment<D, T>, 6> edges() const
    {
        return {{segment<D, T>(pos0, pos1), segment<D, T>(pos0, pos2), segment<D, T>(pos0, pos3),
                 segment<D, T>(pos1, pos2), segment<D, T>(pos1, pos3), segment<D, T>(pos2, pos3)}};
    }
    /// face i is the one opposite vertex i.
    [[nodiscard]] constexpr cc::fixed_array<triangle<D, T>, 4> faces() const
    {
        return {{triangle<D, T>(pos1, pos2, pos3), triangle<D, T>(pos0, pos2, pos3), triangle<D, T>(pos0, pos1, pos3),
                 triangle<D, T>(pos0, pos1, pos2)}};
    }

    // parameters
public:
    /// the point with barycentric coordinates b over pos0 .. pos3.
    [[nodiscard]] constexpr pos<D, T> at(comp<4, T> const& b) const
    {
        return pos0 + (pos1 - pos0) * b.data[1] + (pos2 - pos0) * b.data[2] + (pos3 - pos0) * b.data[3];
    }
    /// the barycentric coordinates of p, unclamped: negative outside.
    [[nodiscard]] constexpr comp<4, T> parameter_of(pos<D, T> const& p) const
    {
        auto const m = mat<3, 3, T>::make_from_cols(pos1 - pos0, pos2 - pos0, pos3 - pos0);
        auto const l = m.inverse() * (p - pos0);
        return comp<4, T>(T(1) - l.data[0] - l.data[1] - l.data[2], l.data[0], l.data[1], l.data[2]);
    }

    // sampling
public:
    /// a point of the unit cube folded into the corner tetrahedron, then mapped: three draws, no rejection.
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto s = rng.uniform(T(0), T(1));
        auto t = rng.uniform(T(0), T(1));
        auto u = rng.uniform(T(0), T(1));
        if (s + t > T(1))
        {
            s = T(1) - s;
            t = T(1) - t;
        }
        if (t + u > T(1))
        {
            auto const old_u = u;
            u = T(1) - s - t;
            t = T(1) - old_u;
        }
        else if (s + t + u > T(1))
        {
            auto const old_u = u;
            u = s + t + u - T(1);
            s = T(1) - t - old_u;
        }
        return pos0 + (pos1 - pos0) * s + (pos2 - pos0) * t + (pos3 - pos0) * u;
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
    [[nodiscard]] friend constexpr bool operator==(tetrahedron const&, tetrahedron const&) = default;
};

/// The four faces of a tg::tetrahedron, intrinsic_dim 2.
template <int D, class T>
struct tg::tetrahedron_boundary
{
    static_assert(D == 3, "a tetrahedron is 3D");

    pos<D, T> pos0;
    pos<D, T> pos1;
    pos<D, T> pos2;
    pos<D, T> pos3;

    // construction
public:
    tetrahedron_boundary() = default;

    explicit constexpr tetrahedron_boundary(pos<D, T> const& p0,
                                            pos<D, T> const& p1,
                                            pos<D, T> const& p2,
                                            pos<D, T> const& p3)
      : pos0(p0), pos1(p1), pos2(p2), pos3(p3)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr tetrahedron<D, T> solid() const { return tetrahedron<D, T>(pos0, pos1, pos2, pos3); }

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
    [[nodiscard]] constexpr T area() const
        requires(tg::traits::has_sqrt<T>)
    {
        return this->solid().area();
    }
    [[nodiscard]] constexpr aabb<D, T> bounds() const { return tg::impl::bounds_of(pos0, pos1, pos2, pos3); }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return pos0; }

    // sampling
public:
    /// a face chosen by its area, then a point on it.
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto const f = this->solid().faces();
        T const a[4] = {f[0].area(), f[1].area(), f[2].area(), f[3].area()};
        auto pick = rng.uniform(T(0), a[0] + a[1] + a[2] + a[3]);
        for (int i = 0; i < 3; ++i)
        {
            if (pick < a[i])
                return f[i].sample_uniform(rng);
            pick = pick - a[i];
        }
        return f[3].sample_uniform(rng);
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
    [[nodiscard]] friend constexpr bool operator==(tetrahedron_boundary const&, tetrahedron_boundary const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::tetrahedron<D, T>>
{
    static constexpr int intrinsic_dim = 3;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

template <int D, class T>
struct tg::object_traits<tg::tetrahedron_boundary<D, T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
