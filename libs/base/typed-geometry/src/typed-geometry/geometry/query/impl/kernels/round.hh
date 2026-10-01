#pragma once

#include <typed-geometry/geometry/primitives/capsule.hh>
#include <typed-geometry/geometry/primitives/cylinder.hh>
#include <typed-geometry/geometry/query/impl/kernels/parameters.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/geometry/query/impl/special_case.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>

/// Kernels for the round objects swept along an axis segment: the capsule, and the cylinder with its surface and tube.

namespace tg::impl
{
/// the parameters of a linear object within distance r of the infinite line a + s u, unclipped.
/// A direction along the axis is handled exactly: all of it or nothing.
template <int D, class T>
[[nodiscard]] constexpr cc::optional<hit_interval<T>> infinite_cylinder(linear_view<D, T> const& v,
                                                                        pos<D, T> const& a,
                                                                        vec<D, T> const& u,
                                                                        T r)
{
    auto const uu = tg::dot(u, u);
    auto const w = v.origin - a;
    auto const dp = v.dir - u * (tg::dot(v.dir, u) / uu);
    auto const wp = w - u * (tg::dot(w, u) / uu);
    auto const qa = tg::dot(dp, dp);
    auto const qb = tg::dot(wp, dp);
    auto const qc = tg::dot(wp, wp) - r * r;
    auto const inf = impl::unbounded_parameter<T>();
    if (tg::traits::is_zero(qa))
        return qc <= T(0) ? cc::optional<hit_interval<T>>(hit_interval<T>{.start = -inf, .end = inf})
                          : cc::optional<hit_interval<T>>();
    auto const disc = qb * qb - qa * qc;
    if (disc < T(0))
        return {};
    auto const s = tg::sqrt(disc);
    return hit_interval<T>{.start = (-qb - s) / qa, .end = (-qb + s) / qa};
}

/// the parameters whose foot on the axis falls within the segment a .. a + u, unclipped.
template <class T>
[[nodiscard]] constexpr cc::optional<hit_interval<T>> axis_slab(linear_view<3, T> const& v,
                                                                pos<3, T> const& a,
                                                                vec<3, T> const& u)
{
    auto const uu = tg::dot(u, u);
    auto const s0 = tg::dot(v.origin - a, u) / uu;
    auto const ds = tg::dot(v.dir, u) / uu;
    auto const inf = impl::unbounded_parameter<T>();
    if (tg::traits::is_zero(ds))
        return (s0 >= T(0) && s0 <= T(1)) ? cc::optional<hit_interval<T>>(hit_interval<T>{.start = -inf, .end = inf})
                                          : cc::optional<hit_interval<T>>();
    auto t0 = -s0 / ds;
    auto t1 = (T(1) - s0) / ds;
    if (t0 > t1)
    {
        auto const t = t0;
        t0 = t1;
        t1 = t;
    }
    return hit_interval<T>{.start = t0, .end = t1};
}

/// the finite cylinder's interval, unclipped: the infinite tube's, cut by the slab between the caps.
template <class T>
[[nodiscard]] constexpr cc::optional<hit_interval<T>> finite_cylinder(linear_view<3, T> const& v,
                                                                      segment<3, T> const& axis,
                                                                      T r)
{
    auto const u = axis.pos1 - axis.pos0;
    auto const tube = impl::infinite_cylinder(v, axis.pos0, u, r);
    auto const slab = impl::axis_slab(v, axis.pos0, u);
    if (!tube.has_value() || !slab.has_value())
        return {};
    auto const a = tube.value().start > slab.value().start ? tube.value().start : slab.value().start;
    auto const b = tube.value().end < slab.value().end ? tube.value().end : slab.value().end;
    if (!(a <= b))
        return {};
    return hit_interval<T>{.start = a, .end = b};
}

/// the capsule's interval, unclipped: it is convex, so the hull of the tube's part and both end balls' intervals.
template <class T>
[[nodiscard]] constexpr cc::optional<hit_interval<T>> capsule_interval(linear_view<3, T> const& v,
                                                                       segment<3, T> const& axis,
                                                                       T r)
{
    cc::optional<hit_interval<T>> parts[3]
        = {impl::finite_cylinder(v, axis, r), impl::sphere_roots(v.origin, v.dir, axis.pos0, r),
           impl::sphere_roots(v.origin, v.dir, axis.pos1, r)};
    auto any = false;
    auto r0 = T(0);
    auto r1 = T(0);
    for (auto const& p : parts)
    {
        if (!p.has_value())
            continue;
        r0 = !any || p.value().start < r0 ? p.value().start : r0;
        r1 = !any || p.value().end > r1 ? p.value().end : r1;
        any = true;
    }
    if (!any)
        return {};
    return hit_interval<T>{.start = r0, .end = r1};
}
} // namespace tg::impl

// --- capsule

/// The axis's support pushed out by the radius along the direction.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::support_op<tg::capsule<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(capsule<D, T> const& c, vec<D, T> const& dir)
    {
        auto const p = tg::dot(c.axis.pos1 - c.axis.pos0, dir) > T(0) ? c.axis.pos1 : c.axis.pos0;
        auto const l = dir.length();
        return tg::traits::is_zero(l) ? p : p + dir * (c.radius / l);
    }
};

/// Onto the axis, then out to the radius when the point is farther than that.
template <int D, class T>
    requires(tg::traits::has_sqrt<T> && !tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::capsule<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, capsule<D, T> const& c)
    {
        auto const q = c.axis.at(c.axis.parameter_of(p));
        auto const v = p - q;
        auto const l2 = v.length_sqr();
        return l2 <= c.radius * c.radius ? p : q + v * (c.radius / tg::sqrt(l2));
    }
};

/// Out to the radius from the axis; a point on the axis is the special case.
template <int D, class T>
    requires(tg::traits::has_sqrt<T> && !tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::capsule_boundary<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, capsule_boundary<D, T> const& c)
    {
        auto const q = c.axis.at(c.axis.parameter_of(p));
        auto const v = p - q;
        auto const l = v.length();
        TG_SPECIAL_CASE(tg::traits::is_zero(l), "projecting a point of a capsule's axis onto its surface");
        return q + v * (c.radius / l);
    }
};

template <int D, class T>
    requires(tg::traits::has_sqrt<T> && !tg::traits::is_exact<T>)
struct tg::impl::signed_distance_op<tg::pos<D, T>, tg::capsule<D, T>>
{
    [[nodiscard]] static constexpr T apply(pos<D, T> const& p, capsule<D, T> const& c)
    {
        return (p - c.axis.at(c.axis.parameter_of(p))).length() - c.radius;
    }
};

template <class L, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::capsule<3, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, capsule<3, T> const& c)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::capsule_interval(v, c.axis, c.radius);
        if (!r.has_value())
            return {};
        return impl::clip(v, r.value().start, r.value().end);
    }
};

template <class L, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::capsule_boundary<3, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, capsule_boundary<3, T> const& c)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::capsule_interval(v, c.axis, c.radius);
        if (!r.has_value())
            return {};
        return impl::crossings<2>(v, r.value().start, r.value().end);
    }
};

// --- cylinder

/// The end farther along the direction, pushed out by the radius along the direction's part perpendicular to the axis.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::support_op<tg::cylinder<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(cylinder<D, T> const& c, vec<D, T> const& dir)
    {
        auto const u = c.axis.pos1 - c.axis.pos0;
        auto const p = tg::dot(u, dir) > T(0) ? c.axis.pos1 : c.axis.pos0;
        auto const perp = dir - u * (tg::dot(dir, u) / tg::dot(u, u));
        auto const l = perp.length();
        return tg::traits::is_zero(l) ? p : p + perp * (c.radius / l);
    }
};

namespace tg::impl
{
/// a point in the cylinder's own terms: the axis parameter s (0 at pos0, 1 at pos1) and the radial offset from the axis.
template <class T>
struct cylinder_coords
{
    T s;
    vec<3, T> radial;
    T rho;
};

template <class T>
[[nodiscard]] constexpr cylinder_coords<T> cylinder_coords_of(pos<3, T> const& p, segment<3, T> const& axis)
{
    auto const u = axis.pos1 - axis.pos0;
    auto const w = p - axis.pos0;
    auto const s = tg::dot(w, u) / tg::dot(u, u);
    auto const radial = w - u * s;
    return {s, radial, radial.length()};
}
} // namespace tg::impl

/// Clamp the axis parameter to the caps and the radial distance to the radius.
template <int D, class T>
    requires(tg::traits::has_sqrt<T> && !tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::cylinder<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, cylinder<D, T> const& c)
    {
        auto const k = impl::cylinder_coords_of(p, c.axis);
        // a point inside is its own projection, returned as given so contains can rely on equality
        if (k.s >= T(0) && k.s <= T(1) && k.rho <= c.radius)
            return p;
        auto const s = impl::clamp01(k.s);
        auto const radial = k.rho <= c.radius ? k.radial : k.radial * (c.radius / k.rho);
        return c.axis.at(s) + radial;
    }
};

/// Outside, the solid's projection lands on the surface already; inside, the nearest of the two caps and the tube wins.
template <int D, class T>
    requires(tg::traits::has_sqrt<T> && !tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::cylinder_boundary<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, cylinder_boundary<D, T> const& c)
    {
        auto const k = impl::cylinder_coords_of(p, c.axis);
        if (k.s < T(0) || k.s > T(1) || k.rho > c.radius)
            return project_op<pos<D, T>, cylinder<D, T>>::apply(p, c.solid());

        auto const h = c.axis.length();
        auto const to_cap0 = k.s * h;
        auto const to_cap1 = (T(1) - k.s) * h;
        auto const to_tube = c.radius - k.rho;
        if (to_tube <= to_cap0 && to_tube <= to_cap1)
        {
            TG_SPECIAL_CASE(tg::traits::is_zero(k.rho), "projecting a point of a cylinder's axis onto its tube");
            return c.axis.at(k.s) + k.radial * (c.radius / k.rho);
        }
        return c.axis.at(to_cap0 <= to_cap1 ? T(0) : T(1)) + k.radial;
    }
};

/// The axis parameter clamped to the tube's extent, then out to the radius; a point on the axis is the special case.
template <int D, class T>
    requires(tg::traits::has_sqrt<T> && !tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::cylinder_mantle<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, cylinder_mantle<D, T> const& c)
    {
        auto const k = impl::cylinder_coords_of(p, c.axis);
        TG_SPECIAL_CASE(tg::traits::is_zero(k.rho), "projecting a point of a cylinder's axis onto its tube");
        return c.axis.at(impl::clamp01(k.s)) + k.radial * (c.radius / k.rho);
    }
};

template <class L, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::cylinder<3, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, cylinder<3, T> const& c)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::finite_cylinder(v, c.axis, c.radius);
        if (!r.has_value())
            return {};
        return impl::clip(v, r.value().start, r.value().end);
    }
};

template <class L, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::cylinder_boundary<3, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, cylinder_boundary<3, T> const& c)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::finite_cylinder(v, c.axis, c.radius);
        if (!r.has_value())
            return {};
        return impl::crossings<2>(v, r.value().start, r.value().end);
    }
};

/// The open tube is crossed where the infinite tube is, at parameters whose foot lies between the caps.
template <class L, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::cylinder_mantle<3, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, cylinder_mantle<3, T> const& c)
    {
        auto const v = impl::linear_of(l);
        auto const u = c.axis.pos1 - c.axis.pos0;
        auto const tube = impl::infinite_cylinder(v, c.axis.pos0, u, c.radius);
        hits<2, T> r;
        if (!tube.has_value())
            return r;
        auto const uu = tg::dot(u, u);
        auto const in_range = [&](T t)
        {
            auto const s = tg::dot(v.origin + v.dir * t - c.axis.pos0, u) / uu;
            return v.lo <= t && t <= v.hi && s >= T(0) && s <= T(1);
        };
        if (in_range(tube.value().start))
            r.add(tube.value().start);
        if (tube.value().end != tube.value().start && in_range(tube.value().end))
            r.add(tube.value().end);
        return r;
    }
};
