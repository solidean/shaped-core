#pragma once

#include <typed-geometry/geometry/primitives/frustum.hh>
#include <typed-geometry/geometry/primitives/infinite.hh>
#include <typed-geometry/geometry/query/impl/kernels/capped.hh>
#include <typed-geometry/geometry/query/impl/kernels/linear.hh>
#include <typed-geometry/geometry/query/impl/kernels/parameters.hh>
#include <typed-geometry/geometry/query/impl/kernels/round.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/geometry/query/impl/special_case.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>

/// Kernels for the unbounded round objects — the infinite cylinder and cone — and the frustum.
/// The unbounded ones have no support, so every query they answer is a closed form here.

namespace tg::impl
{
/// p split against a line: the foot on it, and the offset from that foot.
template <int D, class T>
[[nodiscard]] constexpr cc::pair<pos<D, T>, vec<D, T>> foot_on(pos<D, T> const& p, line<D, T> const& l)
{
    auto const foot = l.origin + l.dir * (tg::dot(p - l.origin, l.dir) / tg::dot(l.dir, l.dir));
    return {foot, p - foot};
}

/// tan of half the opening angle: the cone's radius per unit of height.
template <class T>
[[nodiscard]] constexpr T cone_slope(angle<T> opening)
{
    return tg::tan(opening / T(2));
}

/// the parameters inside a single infinite nappe {rho <= slope * z, z >= 0}, unclipped; one interval, as it is convex.
template <int D, class T>
[[nodiscard]] constexpr cc::optional<hit_interval<T>> inf_cone_interval(linear_view<D, T> const& v,
                                                                        pos<D, T> const& apex,
                                                                        vec<D, T> const& u,
                                                                        T slope)
{
    auto const k2 = slope * slope;
    auto const w = v.origin - apex;
    auto const z0 = tg::dot(w, u);
    auto const dz = tg::dot(v.dir, u);
    auto const wp = w - u * z0;
    auto const dp = v.dir - u * dz;
    auto const a = tg::dot(dp, dp) - k2 * dz * dz;
    auto const b = tg::dot(wp, dp) - k2 * z0 * dz;
    auto const c = tg::dot(wp, wp) - k2 * z0 * z0;
    auto const inf = impl::unbounded_parameter<T>();

    hit_interval<T> pieces[2] = {};
    auto np = 0;
    if (tg::traits::is_zero(a))
    {
        if (tg::traits::is_zero(b))
        {
            if (c <= T(0))
                pieces[np++] = {-inf, inf};
        }
        else
        {
            auto const t = -c / (T(2) * b);
            pieces[np++] = b > T(0) ? hit_interval<T>{-inf, t} : hit_interval<T>{t, inf};
        }
    }
    else
    {
        auto const disc = b * b - a * c;
        if (disc >= T(0))
        {
            auto const s = tg::sqrt(disc);
            auto t0 = (-b - s) / a;
            auto t1 = (-b + s) / a;
            if (t0 > t1)
            {
                auto const t = t0;
                t0 = t1;
                t1 = t;
            }
            if (a > T(0))
                pieces[np++] = {t0, t1};
            else
            {
                pieces[np++] = {-inf, t0};
                pieces[np++] = {t1, inf};
            }
        }
        else if (a < T(0))
            pieces[np++] = {-inf, inf};
    }

    // z >= 0 keeps the nappe the direction points into
    auto s0 = -inf;
    auto s1 = inf;
    if (tg::traits::is_zero(dz))
    {
        if (z0 < T(0))
            return {};
    }
    else if (dz > T(0))
        s0 = -z0 / dz;
    else
        s1 = -z0 / dz;

    auto any = false;
    auto r0 = T(0);
    auto r1 = T(0);
    for (auto i = 0; i < np; ++i)
    {
        auto const lo = pieces[i].start > s0 ? pieces[i].start : s0;
        auto const hi = pieces[i].end < s1 ? pieces[i].end : s1;
        if (!(lo <= hi))
            continue;
        r0 = !any || lo < r0 ? lo : r0;
        r1 = !any || hi > r1 ? hi : r1;
        any = true;
    }
    if (!any)
        return {};
    return hit_interval<T>{.start = r0, .end = r1};
}

/// the crossings an unbounded interval stands for: only its finite ends, in the view's range.
template <int D, class T>
[[nodiscard]] constexpr hits<2, T> finite_ends(linear_view<D, T> const& v, hit_interval<T> const& r)
{
    auto const inf = impl::unbounded_parameter<T>();
    hits<2, T> out;
    auto const add = [&](T t)
    {
        if (t != inf && t != -inf && v.lo <= t && t <= v.hi)
            out.add(t);
    };
    add(r.start);
    if (r.end != r.start)
        add(r.end);
    return out;
}
} // namespace tg::impl

// --- infinite cylinder

template <int D, class T>
    requires(tg::traits::has_sqrt<T> && !tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::inf_cylinder<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, inf_cylinder<D, T> const& c)
    {
        auto const [foot, off] = impl::foot_on(p, c.axis);
        auto const l2 = off.length_sqr();
        return l2 <= c.radius * c.radius ? p : foot + off * (c.radius / tg::sqrt(l2));
    }
};

template <int D, class T>
    requires(tg::traits::has_sqrt<T> && !tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::inf_cylinder_boundary<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, inf_cylinder_boundary<D, T> const& c)
    {
        auto const [foot, off] = impl::foot_on(p, c.axis);
        return foot + impl::radial_offset(off, off.length(), c.axis.dir, c.radius);
    }
};

template <int D, class T>
    requires(tg::traits::has_sqrt<T> && !tg::traits::is_exact<T>)
struct tg::impl::signed_distance_op<tg::pos<D, T>, tg::inf_cylinder<D, T>>
{
    [[nodiscard]] static constexpr T apply(pos<D, T> const& p, inf_cylinder<D, T> const& c)
    {
        return impl::foot_on(p, c.axis).second.length() - c.radius;
    }
};

/// A ball meets the infinite cylinder when its center is within both radii of the axis.
template <int D, class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::intersects_op<tg::inf_cylinder<D, T>, tg::sphere<D, D, T>>
{
    [[nodiscard]] static constexpr bool apply(inf_cylinder<D, T> const& c, sphere<D, D, T> const& s)
    {
        auto const r = c.radius + s.radius;
        return impl::foot_on(s.center, c.axis).second.length_sqr() <= r * r;
    }
};

template <class L, int D, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::inf_cylinder<D, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, inf_cylinder<D, T> const& c)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::infinite_cylinder(v, c.axis.origin, c.axis.dir, c.radius);
        if (!r.has_value())
            return {};
        return impl::clip(v, r.value().start, r.value().end);
    }
};

template <class L, int D, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::inf_cylinder_boundary<D, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, inf_cylinder_boundary<D, T> const& c)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::infinite_cylinder(v, c.axis.origin, c.axis.dir, c.radius);
        if (!r.has_value())
            return {};
        return impl::finite_ends(v, r.value());
    }
};

// --- infinite cone

/// Within half the opening angle of the direction: the axial component reaches the cosine of it.
template <int D, class T>
    requires(tg::traits::has_sqrt<T> && tg::traits::has_trigonometry<T>)
struct tg::impl::contains_op<tg::inf_cone<D, T>, tg::pos<D, T>>
{
    [[nodiscard]] static constexpr bool apply(inf_cone<D, T> const& c, pos<D, T> const& p)
    {
        auto const w = p - c.apex;
        auto const z = tg::dot(w, c.dir);
        return z >= T(0) && (w - c.dir * z).length() <= impl::cone_slope(c.opening_angle) * z;
    }
};

/// In the profile half-plane the cone is a wedge from the apex; outside it, the nearest point is on its edge ray.
template <int D, class T>
    requires(D == 3 && tg::traits::has_sqrt<T> && tg::traits::has_trigonometry<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::inf_cone<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, inf_cone<D, T> const& c)
    {
        if (contains_op<inf_cone<D, T>, pos<D, T>>::apply(c, p))
            return p;
        return project_op<pos<D, T>, inf_cone_boundary<D, T>>::apply(p, c.boundary());
    }
};

template <int D, class T>
    requires(D == 3 && tg::traits::has_sqrt<T> && tg::traits::has_trigonometry<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::inf_cone_boundary<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, inf_cone_boundary<D, T> const& c)
    {
        auto const k = impl::profile_of(p, c.apex, c.dir);
        auto const edge = ray<2, T>(pos<2, T>(T(0), T(0)), vec<2, T>(T(1), impl::cone_slope(c.opening_angle)));
        return impl::from_profile(k, c.apex, project_op<pos<2, T>, ray<2, T>>::apply(pos<2, T>(k.z, k.rho), edge));
    }
};

template <class L, int D, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T> && tg::traits::has_trigonometry<T>)
struct tg::impl::intersection_parameter_op<L, tg::inf_cone<D, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, inf_cone<D, T> const& c)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::inf_cone_interval(v, c.apex, c.dir, impl::cone_slope(c.opening_angle));
        if (!r.has_value())
            return {};
        return impl::clip(v, r.value().start, r.value().end);
    }
};

template <class L, int D, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T> && tg::traits::has_trigonometry<T>)
struct tg::impl::intersection_parameter_op<L, tg::inf_cone_boundary<D, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, inf_cone_boundary<D, T> const& c)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::inf_cone_interval(v, c.apex, c.dir, impl::cone_slope(c.opening_angle));
        if (!r.has_value())
            return {};
        return impl::finite_ends(v, r.value());
    }
};

// --- frustum

/// The farthest of its eight corners.
/// A frustum without a far plane is unbounded and has no support.
/// It is a special case: its far corners are NaN, so this answers for the near rectangle.
template <int D, class T>
struct tg::impl::support_op<tg::frustum<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(frustum<D, T> const& f, vec<D, T> const& dir)
    {
        TG_SPECIAL_CASE(!f.has_far_plane(), "the support of a frustum without a far plane");
        auto const v = f.vertices();
        auto best = 0;
        auto best_d = tg::dot(v[0] - pos<D, T>(), dir);
        for (int i = 1; i < 8; ++i)
        {
            auto const d = tg::dot(v[i] - pos<D, T>(), dir);
            if (d > best_d)
            {
                best = i;
                best_d = d;
            }
        }
        return v[best];
    }
};

/// Inside every plane.
template <int D, class T>
struct tg::impl::contains_op<tg::frustum<D, T>, tg::pos<D, T>>
{
    [[nodiscard]] static constexpr bool apply(frustum<D, T> const& f, pos<D, T> const& p)
    {
        for (auto const& pl : f.planes)
            if (tg::dot(pl.normal, p - pos<D, T>()) > pl.dist)
                return false;
        return true;
    }
};

/// Inside every plane and exactly on one of them; an absent far plane bounds nothing, so a point is never on it.
template <int D, class T>
struct tg::impl::contains_op<tg::frustum_boundary<D, T>, tg::pos<D, T>>
{
    [[nodiscard]] static constexpr bool apply(frustum_boundary<D, T> const& f, pos<D, T> const& p)
    {
        auto const far_present = f.solid().has_far_plane();
        auto on_plane = false;
        for (int i = 0; i < 6; ++i)
        {
            auto const& pl = f.planes[i];
            auto const d = tg::dot(pl.normal, p - pos<D, T>());
            if (d > pl.dist)
                return false;
            on_plane = on_plane || (d == pl.dist && (i < 5 || far_present));
        }
        return on_plane;
    }
};

namespace tg::impl
{
/// the parameters inside every plane of a frustum, unclipped.
template <class T>
[[nodiscard]] constexpr cc::optional<hit_interval<T>> frustum_interval(linear_view<3, T> const& v, frustum<3, T> const& f)
{
    auto const inf = impl::unbounded_parameter<T>();
    auto t0 = -inf;
    auto t1 = inf;
    for (auto const& pl : f.planes)
    {
        auto const off = tg::dot(pl.normal, v.origin - pos<3, T>()) - pl.dist;
        auto const rate = tg::dot(pl.normal, v.dir);
        if (tg::traits::is_zero(rate))
        {
            if (off > T(0))
                return {};
            continue;
        }
        auto const t = -off / rate;
        if (rate > T(0))
            t1 = t < t1 ? t : t1;
        else
            t0 = t > t0 ? t : t0;
    }
    if (!(t0 <= t1))
        return {};
    return hit_interval<T>{.start = t0, .end = t1};
}
} // namespace tg::impl

template <class L, class T>
    requires(tg::impl::is_linear<L> && !tg::traits::is_exact<T>)
struct tg::impl::intersection_parameter_op<L, tg::frustum<3, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, frustum<3, T> const& f)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::frustum_interval(v, f);
        if (!r.has_value())
            return {};
        return impl::clip(v, r.value().start, r.value().end);
    }
};

template <class L, class T>
    requires(tg::impl::is_linear<L> && !tg::traits::is_exact<T>)
struct tg::impl::intersection_parameter_op<L, tg::frustum_boundary<3, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, frustum_boundary<3, T> const& f)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::frustum_interval(v, f.solid());
        if (!r.has_value())
            return {};
        // without a far plane the interval can end at infinity, which is not a crossing
        return impl::finite_ends(v, r.value());
    }
};

/// The culling test: an object wholly outside any one plane is certainly apart.
/// Near a corner an object can be outside the frustum while straddling every plane, which is why this may say true.
template <int D, class T, class Obj>
    requires(tg::impl::has_support<Obj>)
struct tg::impl::may_intersect_op<tg::frustum<D, T>, Obj>
{
    [[nodiscard]] static constexpr bool apply(frustum<D, T> const& f, Obj const& o)
    {
        for (auto const& pl : f.planes)
            if (tg::dot(pl.normal, support_op<Obj>::apply(o, -pl.normal) - pos<D, T>()) > pl.dist)
                return false;
        return true;
    }
};
