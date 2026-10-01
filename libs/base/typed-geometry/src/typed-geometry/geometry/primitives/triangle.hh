#pragma once

#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/impl/bounds_of.hh>
#include <typed-geometry/geometry/impl/sampling.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/plane.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/comp.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Triangle (filled) with D-dimensional vertices.
///
/// Represents the solid triangle — the convex hull of its three vertices.
/// That is the set of points {a*pos0 + b*pos1 + c*pos2 : a,b,c >= 0, a+b+c == 1}.
/// It is a 2D surface patch (intrinsic_dim == 2) living in D-dimensional space (ambient_dim == D), and it is finite.
/// A triangle whose three vertices are collinear is degenerate, which is not enforced against at construction.
///
///     tg::triangle3f t(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0));
template <int D, class T>
struct tg::triangle
{
    static_assert(D > 0, "triangle requires a positive dimension");

    pos<D, T> pos0;
    pos<D, T> pos1;
    pos<D, T> pos2;

    // construction
public:
    triangle() = default;

    explicit constexpr triangle(pos<D, T> const& pos0, pos<D, T> const& pos1, pos<D, T> const& pos2)
      : pos0(pos0), pos1(pos1), pos2(pos2)
    {
    }

    // transformation
public:
    /// An affine map sends the convex hull of the vertices to the convex hull of their images.
    ///
    /// A projective map does too, but only while every vertex stays in front of the projection.
    /// That is NOT checked: a vertex behind it maps to its mirror image, and the result is a triangle over
    /// the wrong points rather than a diagnosed error.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::affine_transform<D, T>(t); })
        {
            auto const a = tg::affine_transform<D, T>(t);
            return triangle(pos0.transformed(a), pos1.transformed(a), pos2.transformed(a));
        }
        else if constexpr (requires { tg::projective_transform<D, T>(t); })
        {
            auto const p = tg::projective_transform<D, T>(t);
            return triangle(pos0.transformed(p), pos1.transformed(p), pos2.transformed(p));
        }
        else
            static_assert(false, "tg: a triangle can be transformed by an affine or a projective map");
    }

    // measures and readings
public:
    /// half the parallelogram the two edges at pos0 span; in 2D exact for every scalar with abs.
    [[nodiscard]] constexpr T area() const
    {
        auto const ab = pos1 - pos0;
        auto const ac = pos2 - pos0;
        if constexpr (D == 2)
            return tg::abs(ab.data[0] * ac.data[1] - ab.data[1] * ac.data[0]) / T(2);
        else
        {
            auto const d = tg::dot(ab, ac);
            return tg::sqrt(ab.length_sqr() * ac.length_sqr() - d * d) / T(2);
        }
    }
    [[nodiscard]] constexpr T perimeter() const
        requires(tg::traits::has_sqrt<T>)
    {
        return (pos1 - pos0).length() + (pos2 - pos1).length() + (pos0 - pos2).length();
    }
    [[nodiscard]] constexpr pos<D, T> centroid() const { return pos0 + ((pos1 - pos0) + (pos2 - pos0)) / T(3); }
    [[nodiscard]] constexpr aabb<D, T> bounds() const { return tg::impl::bounds_of(pos0, pos1, pos2); }
    [[nodiscard]] constexpr cc::fixed_array<pos<D, T>, 3> vertices() const { return {{pos0, pos1, pos2}}; }
    [[nodiscard]] constexpr cc::fixed_array<segment<D, T>, 3> edges() const
    {
        return {{segment<D, T>(pos0, pos1), segment<D, T>(pos1, pos2), segment<D, T>(pos2, pos0)}};
    }
    /// unit normal, oriented by the vertex order (counter-clockwise seen from where it points).
    [[nodiscard]] constexpr vec<3, T> normal() const
        requires(D == 3 && tg::traits::has_sqrt<T>)
    {
        return tg::dual(tg::cross(pos1 - pos0, pos2 - pos0)).normalized();
    }
    [[nodiscard]] constexpr tg::plane<3, T> plane() const
        requires(D == 3 && tg::traits::has_sqrt<T>)
    {
        auto const n = this->normal();
        return tg::plane<3, T>(n, tg::dot(n, pos0 - pos<3, T>()));
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return pos0; }

    // parameters
public:
    /// the point with barycentric coordinates b; the triangle is b_i >= 0 with b_0 + b_1 + b_2 == 1.
    [[nodiscard]] constexpr pos<D, T> at(comp<3, T> const& b) const
    {
        return pos0 + (pos1 - pos0) * b.data[1] + (pos2 - pos0) * b.data[2];
    }
    /// the barycentric coordinates of p's projection onto the triangle's plane, unclamped: negative outside.
    [[nodiscard]] constexpr comp<3, T> parameter_of(pos<D, T> const& p) const
    {
        auto const ab = pos1 - pos0;
        auto const ac = pos2 - pos0;
        auto const ap = p - pos0;
        auto const d00 = tg::dot(ab, ab);
        auto const d01 = tg::dot(ab, ac);
        auto const d11 = tg::dot(ac, ac);
        auto const d20 = tg::dot(ap, ab);
        auto const d21 = tg::dot(ap, ac);
        auto const denom = d00 * d11 - d01 * d01;
        auto const v = (d11 * d20 - d01 * d21) / denom;
        auto const w = (d00 * d21 - d01 * d20) / denom;
        return comp<3, T>(T(1) - v - w, v, w);
    }

    // sampling
public:
    /// a point of the unit square, folded onto the half below its diagonal: two draws, no rejection.
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto u = rng.uniform(T(0), T(1));
        auto v = rng.uniform(T(0), T(1));
        if (u + v > T(1))
        {
            u = T(1) - u;
            v = T(1) - v;
        }
        return pos0 + (pos1 - pos0) * u + (pos2 - pos0) * v;
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
    [[nodiscard]] friend constexpr bool operator==(triangle const&, triangle const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::triangle<D, T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
