#pragma once

#include <typed-geometry/geometry/primitives/sphere.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/geometry/query/impl/special_case.hh>
#include <typed-geometry/scalar/scalar.hh>

/// Kernels against a ball and its surface, spanning their ambient space.

/// A point inside the ball is its own projection; one outside moves radially onto the surface.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::sphere<D, D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, sphere<D, D, T> const& s)
    {
        auto const v = p - s.center;
        auto const l2 = v.length_sqr();
        if (l2 <= s.radius * s.radius)
            return p;
        return s.center + v * (s.radius / tg::sqrt(l2));
    }
};

/// Every point moves radially onto the surface; the center has no direction to move in.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::sphere_boundary<D, D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, sphere_boundary<D, D, T> const& s)
    {
        auto const v = p - s.center;
        auto const l = v.length();
        TG_SPECIAL_CASE(tg::traits::is_zero(l), "projecting a sphere's center onto its surface");
        return s.center + v * (s.radius / l);
    }
};

/// Negative inside: the distance to the center minus the radius.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::signed_distance_op<tg::pos<D, T>, tg::sphere<D, D, T>>
{
    [[nodiscard]] static constexpr T apply(pos<D, T> const& p, sphere<D, D, T> const& s)
    {
        return (p - s.center).length() - s.radius;
    }
};

/// The surface's inside is the ball it bounds, so it has the ball's signed distance.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::signed_distance_op<tg::pos<D, T>, tg::sphere_boundary<D, D, T>>
{
    [[nodiscard]] static constexpr T apply(pos<D, T> const& p, sphere_boundary<D, D, T> const& s)
    {
        return (p - s.center).length() - s.radius;
    }
};

/// The center pushed out by the radius along the direction; a zero direction has no farthest point, so it is the center.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::support_op<tg::sphere<D, D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(sphere<D, D, T> const& s, vec<D, T> const& dir)
    {
        auto const l = dir.length();
        if (tg::traits::is_zero(l))
            return s.center;
        return s.center + dir * (s.radius / l);
    }
};
