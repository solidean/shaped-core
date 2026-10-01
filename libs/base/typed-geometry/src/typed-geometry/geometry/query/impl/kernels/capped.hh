#pragma once

#include <typed-geometry/geometry/primitives/cone.hh>
#include <typed-geometry/geometry/primitives/hemisphere.hh>
#include <typed-geometry/geometry/query/impl/gjk.hh>
#include <typed-geometry/geometry/query/impl/kernels/linear.hh>
#include <typed-geometry/geometry/query/impl/kernels/parameters.hh>
#include <typed-geometry/geometry/query/impl/kernels/triangle.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/geometry/query/impl/special_case.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>

/// Kernels for the capped round objects: the cone and the hemisphere, each with its whole surface and its mantle.

namespace tg::impl
{
/// a point of 3D space in a cone's or cylinder's profile: z along the unit axis from the origin, rho away from it.
template <class T>
struct profile
{
    vec<3, T> u;      // the unit axis
    vec<3, T> radial; // the offset from the axis, of length rho
    T z;
    T rho;
};

template <class T>
[[nodiscard]] constexpr profile<T> profile_of(pos<3, T> const& p, pos<3, T> const& origin, vec<3, T> const& axis)
{
    auto const u = axis.normalized();
    auto const w = p - origin;
    auto const z = tg::dot(w, u);
    auto const radial = w - u * z;
    return {u, radial, z, radial.length()};
}

/// a profile point (z', rho') back in 3D, on the same half-plane as the original point.
/// A point on the axis has no half-plane; any direction around the axis serves, and one with rho' = 0 needs none.
template <class T>
[[nodiscard]] constexpr pos<3, T> from_profile(profile<T> const& k, pos<3, T> const& origin, pos<2, T> const& q)
{
    auto const around = tg::traits::is_zero(k.rho) ? tg::any_orthogonal(k.u).normalized() : k.radial / k.rho;
    return origin + k.u * q.data[0] + around * q.data[1];
}

/// the parameters inside the solid single cone {rho <= slope * z, 0 <= z <= h}, unclipped.
/// The cone is convex, so the set is one interval: the pieces where the quadric is non-positive, cut to the slab.
template <class T>
[[nodiscard]] constexpr cc::optional<hit_interval<T>> cone_interval(linear_view<3, T> const& v,
                                                                    pos<3, T> const& apex,
                                                                    vec<3, T> const& axis,
                                                                    T radius)
{
    auto const h = axis.length();
    auto const u = axis / h;
    auto const k2 = (radius / h) * (radius / h);
    auto const w = v.origin - apex;
    auto const z0 = tg::dot(w, u);
    auto const dz = tg::dot(v.dir, u);
    auto const wp = w - u * z0;
    auto const dp = v.dir - u * dz;
    auto const a = tg::dot(dp, dp) - k2 * dz * dz;
    auto const b = tg::dot(wp, dp) - k2 * z0 * dz;
    auto const c = tg::dot(wp, wp) - k2 * z0 * z0;
    auto const inf = impl::unbounded_parameter<T>();

    // the quadric's non-positive set: at most two pieces
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

    // the slab 0 <= z <= h, which also keeps only the nappe in front of the apex
    auto s0 = -inf;
    auto s1 = inf;
    if (tg::traits::is_zero(dz))
    {
        if (z0 < T(0) || z0 > h)
            return {};
    }
    else
    {
        s0 = -z0 / dz;
        s1 = (h - z0) / dz;
        if (s0 > s1)
        {
            auto const t = s0;
            s0 = s1;
            s1 = t;
        }
    }

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
} // namespace tg::impl

// --- cone

/// The apex or the base rim point farthest along the direction, whichever reaches farther.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::support_op<tg::cone<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(cone<D, T> const& c, vec<D, T> const& dir)
    {
        auto const perp = dir - c.axis * (tg::dot(dir, c.axis) / tg::dot(c.axis, c.axis));
        auto const l = perp.length();
        auto const rim = tg::traits::is_zero(l) ? c.apex + c.axis : c.apex + c.axis + perp * (c.radius / l);
        return tg::dot(rim - c.apex, dir) > T(0) ? rim : c.apex;
    }
};

/// In the profile half-plane the cone is the triangle apex, base rim, base center.
template <int D, class T>
    requires(tg::traits::has_sqrt<T> && !tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::cone<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, cone<D, T> const& c)
    {
        auto const k = impl::profile_of(p, c.apex, c.axis);
        auto const h = c.axis.length();
        auto const tri = triangle<2, T>(pos<2, T>(T(0), T(0)), pos<2, T>(h, c.radius), pos<2, T>(h, T(0)));
        auto const profile_p = pos<2, T>(k.z, k.rho);
        auto const q = project_op<pos<2, T>, triangle<2, T>>::apply(profile_p, tri);
        // a point inside is its own projection, returned as given so contains can rely on equality
        return q == profile_p ? p : impl::from_profile(k, c.apex, q);
    }
};

/// Outside, the solid's projection is on the surface already; inside, the nearer of the slant and the base.
template <int D, class T>
    requires(tg::traits::has_sqrt<T> && !tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::cone_boundary<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, cone_boundary<D, T> const& c)
    {
        auto const k = impl::profile_of(p, c.apex, c.axis);
        auto const h = c.axis.length();
        auto const q = pos<2, T>(k.z, k.rho);
        auto const tri = triangle<2, T>(pos<2, T>(T(0), T(0)), pos<2, T>(h, c.radius), pos<2, T>(h, T(0)));
        auto const solid = project_op<pos<2, T>, triangle<2, T>>::apply(q, tri);
        if (solid != q)
            return impl::from_profile(k, c.apex, solid);

        auto const slant = project_op<pos<2, T>, segment<2, T>>::apply(q, segment<2, T>(tri.pos0, tri.pos1));
        auto const base = project_op<pos<2, T>, segment<2, T>>::apply(q, segment<2, T>(tri.pos2, tri.pos1));
        auto const nearer = (slant - q).length_sqr() <= (base - q).length_sqr() ? slant : base;
        return impl::from_profile(k, c.apex, nearer);
    }
};

template <int D, class T>
    requires(tg::traits::has_sqrt<T> && !tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::cone_mantle<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, cone_mantle<D, T> const& c)
    {
        auto const k = impl::profile_of(p, c.apex, c.axis);
        auto const h = c.axis.length();
        auto const slant = segment<2, T>(pos<2, T>(T(0), T(0)), pos<2, T>(h, c.radius));
        return impl::from_profile(k, c.apex, project_op<pos<2, T>, segment<2, T>>::apply(pos<2, T>(k.z, k.rho), slant));
    }
};

template <class L, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::cone<3, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, cone<3, T> const& c)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::cone_interval(v, c.apex, c.axis, c.radius);
        if (!r.has_value())
            return {};
        return impl::clip(v, r.value().start, r.value().end);
    }
};

template <class L, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::cone_boundary<3, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, cone_boundary<3, T> const& c)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::cone_interval(v, c.apex, c.axis, c.radius);
        if (!r.has_value())
            return {};
        return impl::crossings<2>(v, r.value().start, r.value().end);
    }
};

/// The slant is crossed where the solid's interval ends anywhere but on the base plane.
template <class L, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::cone_mantle<3, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, cone_mantle<3, T> const& c)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::cone_interval(v, c.apex, c.axis, c.radius);
        hits<2, T> out;
        if (!r.has_value())
            return out;
        auto const h2 = c.axis.length_sqr();
        auto const on_slant = [&](T t)
        {
            auto const z = tg::dot(v.origin + v.dir * t - c.apex, c.axis) / h2;
            return z < T(1) - T(16) * impl::machine_epsilon<T>();
        };
        auto const add = [&](T t)
        {
            if (v.lo <= t && t <= v.hi && on_slant(t))
                out.add(t);
        };
        add(r.value().start);
        if (r.value().end != r.value().start)
            add(r.value().end);
        return out;
    }
};

// --- hemisphere

/// Above the base the ball's support; below, the base rim point farthest along the direction.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::support_op<tg::hemisphere<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(hemisphere<D, T> const& h, vec<D, T> const& dir)
    {
        auto const up = tg::dot(dir, h.normal);
        auto const d = up >= T(0) ? dir : dir - h.normal * up;
        auto const l = d.length();
        return tg::traits::is_zero(l) ? h.center : h.center + d * (h.radius / l);
    }
};

/// Above the base, the ball's projection; below it, the base disk's, since the half ball lies on one side of it.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::hemisphere<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, hemisphere<D, T> const& h)
    {
        auto const up = tg::dot(p - h.center, h.normal);
        auto const q = up >= T(0) ? p : p - h.normal * up;
        auto const v = q - h.center;
        auto const l2 = v.length_sqr();
        return l2 <= h.radius * h.radius ? q : h.center + v * (h.radius / tg::sqrt(l2));
    }
};

/// Inside, the nearer of the dome and the base; outside, the solid's projection, which is on the surface.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::hemisphere_boundary<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, hemisphere_boundary<D, T> const& h)
    {
        auto const up = tg::dot(p - h.center, h.normal);
        auto const l = (p - h.center).length();
        if (up < T(0) || l > h.radius)
            return project_op<pos<D, T>, hemisphere<D, T>>::apply(p, h.solid());
        if (h.radius - l < up)
        {
            TG_SPECIAL_CASE(tg::traits::is_zero(l), "projecting a hemisphere's center onto its dome");
            return h.center + (p - h.center) * (h.radius / l);
        }
        return p - h.normal * up;
    }
};

/// Above the base, radially out; below it, the rim — the dome's nearest point to anything under its base.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::hemisphere_mantle<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, hemisphere_mantle<D, T> const& h)
    {
        auto const w = p - h.center;
        auto const up = tg::dot(w, h.normal);
        auto const d = up >= T(0) ? w : w - h.normal * up;
        auto const l = d.length();
        if (tg::traits::is_zero(l))
            return up >= T(0) ? h.center + h.normal * h.radius
                              : h.center + tg::any_orthogonal(h.normal).normalized() * h.radius;
        return h.center + d * (h.radius / l);
    }
};

namespace tg::impl
{
/// the half ball's interval: the ball's, cut by the half-space above the base.
template <class T>
[[nodiscard]] constexpr cc::optional<hit_interval<T>> hemisphere_interval(linear_view<3, T> const& v,
                                                                          hemisphere<3, T> const& h)
{
    auto const ball = impl::sphere_roots(v.origin, v.dir, h.center, h.radius);
    if (!ball.has_value())
        return {};
    auto t0 = ball.value().start;
    auto t1 = ball.value().end;
    auto const off = tg::dot(v.origin - h.center, h.normal);
    auto const rate = tg::dot(v.dir, h.normal);
    if (tg::traits::is_zero(rate))
    {
        if (off < T(0))
            return {};
    }
    else
    {
        auto const t = -off / rate;
        if (rate > T(0))
            t0 = t > t0 ? t : t0;
        else
            t1 = t < t1 ? t : t1;
    }
    if (!(t0 <= t1))
        return {};
    return hit_interval<T>{.start = t0, .end = t1};
}
} // namespace tg::impl

template <class L, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::hemisphere<3, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, hemisphere<3, T> const& h)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::hemisphere_interval(v, h);
        if (!r.has_value())
            return {};
        return impl::clip(v, r.value().start, r.value().end);
    }
};

template <class L, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::hemisphere_boundary<3, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, hemisphere_boundary<3, T> const& h)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::hemisphere_interval(v, h.solid());
        if (!r.has_value())
            return {};
        return impl::crossings<2>(v, r.value().start, r.value().end);
    }
};

/// The sphere's crossings that lie on the dome's side of the base.
template <class L, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::hemisphere_mantle<3, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, hemisphere_mantle<3, T> const& h)
    {
        auto const v = impl::linear_of(l);
        auto const ball = impl::sphere_roots(v.origin, v.dir, h.center, h.radius);
        hits<2, T> out;
        if (!ball.has_value())
            return out;
        auto const add = [&](T t)
        {
            if (v.lo <= t && t <= v.hi && tg::dot(v.origin + v.dir * t - h.center, h.normal) >= T(0))
                out.add(t);
        };
        add(ball.value().start);
        if (ball.value().end != ball.value().start)
            add(ball.value().end);
        return out;
    }
};
