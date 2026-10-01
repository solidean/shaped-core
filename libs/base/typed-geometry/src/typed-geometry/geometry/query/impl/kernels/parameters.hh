#pragma once

#include <clean-core/error/optional.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/box.hh>
#include <typed-geometry/geometry/primitives/ellipsoid.hh>
#include <typed-geometry/geometry/primitives/halfspace.hh>
#include <typed-geometry/geometry/primitives/line.hh>
#include <typed-geometry/geometry/primitives/plane.hh>
#include <typed-geometry/geometry/primitives/ray.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/primitives/sphere.hh>
#include <typed-geometry/geometry/primitives/triangle.hh>
#include <typed-geometry/geometry/query/hits.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/geometry/query/impl/special_case.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <limits>

/// Where a line, ray or segment meets an object, as parameters along it.
///
/// All three are `origin + t * dir` over a parameter range — all of t, t >= 0, or [0, 1] — so each target has one
/// kernel over that view, computing the unclipped answer, and the range is applied once.
/// A surface gives `tg::hits` (each crossing), a solid `cc::optional<tg::hit_interval>` (the part inside).

namespace tg::impl
{
template <int D, class T>
struct linear_view
{
    static constexpr int dim = D;

    pos<D, T> origin;
    vec<D, T> dir;
    T lo;
    T hi;
};

/// the scalar's infinity, or its largest value where it has none.
template <class T>
[[nodiscard]] constexpr T unbounded_parameter()
{
    if constexpr (std::numeric_limits<T>::has_infinity)
        return std::numeric_limits<T>::infinity();
    else
        return std::numeric_limits<T>::max();
}

template <int D, class T>
[[nodiscard]] constexpr linear_view<D, T> linear_of(line<D, T> const& l)
{
    return {l.origin, l.dir, -impl::unbounded_parameter<T>(), impl::unbounded_parameter<T>()};
}
template <int D, class T>
[[nodiscard]] constexpr linear_view<D, T> linear_of(ray<D, T> const& r)
{
    return {r.origin, r.dir, T(0), impl::unbounded_parameter<T>()};
}
template <int D, class T>
[[nodiscard]] constexpr linear_view<D, T> linear_of(segment<D, T> const& s)
{
    return {s.pos0, s.pos1 - s.pos0, T(0), T(1)};
}

/// the part of [t0, t1] inside the view's range, if any.
template <int D, class T>
[[nodiscard]] constexpr cc::optional<hit_interval<T>> clip(linear_view<D, T> const& v, T t0, T t1)
{
    auto const a = t0 > v.lo ? t0 : v.lo;
    auto const b = t1 < v.hi ? t1 : v.hi;
    if (!(a <= b))
        return {};
    return hit_interval<T>{.start = a, .end = b};
}

/// the crossings t0 <= t1 that lie in the view's range, a tangent counted once.
template <int N, int D, class T>
[[nodiscard]] constexpr hits<N, T> crossings(linear_view<D, T> const& v, T t0, T t1)
{
    hits<N, T> r;
    if (v.lo <= t0 && t0 <= v.hi)
        r.add(t0);
    if (t1 != t0 && v.lo <= t1 && t1 <= v.hi)
        r.add(t1);
    return r;
}

/// the slab interval of a ray o + t d through the box [-1, 1]^D or [min, max], unclipped; empty when it misses.
/// A direction parallel to a slab is handled exactly rather than through a division by zero.
template <int D, class T>
[[nodiscard]] constexpr cc::optional<hit_interval<T>> slabs(pos<D, T> const& o,
                                                            vec<D, T> const& d,
                                                            pos<D, T> const& lo,
                                                            pos<D, T> const& hi)
{
    auto t0 = -impl::unbounded_parameter<T>();
    auto t1 = impl::unbounded_parameter<T>();
    for (int i = 0; i < D; ++i)
    {
        if (tg::traits::is_zero(d.data[i]))
        {
            if (o.data[i] < lo.data[i] || o.data[i] > hi.data[i])
                return {};
            continue;
        }
        auto a = (lo.data[i] - o.data[i]) / d.data[i];
        auto b = (hi.data[i] - o.data[i]) / d.data[i];
        if (a > b)
        {
            auto const t = a;
            a = b;
            b = t;
        }
        t0 = a > t0 ? a : t0;
        t1 = b < t1 ? b : t1;
    }
    if (!(t0 <= t1))
        return {};
    return hit_interval<T>{.start = t0, .end = t1};
}

/// the two roots of |o + t d - c|^2 = r^2 over the unit-free quadratic, or none.
template <int D, class T>
[[nodiscard]] constexpr cc::optional<hit_interval<T>> sphere_roots(pos<D, T> const& o,
                                                                   vec<D, T> const& d,
                                                                   pos<D, T> const& c,
                                                                   T r)
{
    auto const oc = o - c;
    auto const a = tg::dot(d, d);
    auto const b = tg::dot(oc, d);
    auto const k = tg::dot(oc, oc) - r * r;
    auto const disc = b * b - a * k;
    if (disc < T(0))
        return {};
    auto const s = tg::sqrt(disc);
    return hit_interval<T>{.start = (-b - s) / a, .end = (-b + s) / a};
}

/// a linear object mapped into the unit frame of a box or an ellipsoid: o' = M^-1 (o - c), d' = M^-1 d.
/// Parameters are unchanged by an affine map, which is what lets both reuse the unit-shape kernels.
template <int D, class T>
[[nodiscard]] constexpr linear_view<D, T> into_frame(linear_view<D, T> const& v, pos<D, T> const& c, mat<D, D, T> const& m)
{
    auto const inv = m.inverse();
    return {pos<D, T>() + inv * (v.origin - c), inv * v.dir, v.lo, v.hi};
}
} // namespace tg::impl

// --- plane and half-space

/// One crossing; a direction parallel to the plane is the special case, and gives a non-finite parameter.
template <class L, int D, class T>
    requires tg::impl::is_linear<L>
struct tg::impl::intersection_parameter_op<L, tg::plane<D, T>>
{
    [[nodiscard]] static constexpr hits<1, T> apply(L const& l, plane<D, T> const& pl)
    {
        auto const v = impl::linear_of(l);
        auto const denom = tg::dot(pl.normal, v.dir);
        TG_SPECIAL_CASE(tg::traits::is_zero(denom), "a linear object parallel to a plane");
        auto const t = (pl.dist - tg::dot(pl.normal, v.origin - pos<D, T>())) / denom;
        hits<1, T> r;
        if (v.lo <= t && t <= v.hi)
            r.add(t);
        return r;
    }
};

/// The parameters on the inside: everything up to the crossing or from it, by which way the direction points.
template <class L, int D, class T>
    requires tg::impl::is_linear<L>
struct tg::impl::intersection_parameter_op<L, tg::halfspace<D, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, halfspace<D, T> const& h)
    {
        auto const v = impl::linear_of(l);
        auto const off = tg::dot(h.normal, v.origin - pos<D, T>()) - h.dist;
        auto const denom = tg::dot(h.normal, v.dir);
        auto const inf = impl::unbounded_parameter<T>();
        if (tg::traits::is_zero(denom))
            return off <= T(0) ? impl::clip(v, -inf, inf) : cc::optional<hit_interval<T>>();
        auto const t = -off / denom;
        return denom > T(0) ? impl::clip(v, -inf, t) : impl::clip(v, t, inf);
    }
};

// --- ball and sphere surface

template <class L, int D, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::sphere<D, D, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, sphere<D, D, T> const& s)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::sphere_roots(v.origin, v.dir, s.center, s.radius);
        if (!r.has_value())
            return {};
        return impl::clip(v, r.value().start, r.value().end);
    }
};

template <class L, int D, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::sphere_boundary<D, D, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, sphere_boundary<D, D, T> const& s)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::sphere_roots(v.origin, v.dir, s.center, s.radius);
        if (!r.has_value())
            return {};
        return impl::crossings<2>(v, r.value().start, r.value().end);
    }
};

/// The plane's crossing, kept when it falls inside the disk.
template <class L, class T>
    requires(tg::impl::is_linear<L>)
struct tg::impl::intersection_parameter_op<L, tg::sphere<2, 3, T>>
{
    [[nodiscard]] static constexpr hits<1, T> apply(L const& l, sphere<2, 3, T> const& d)
    {
        auto const v = impl::linear_of(l);
        auto const r = intersection_parameter_op<L, plane<3, T>>::apply(
            l, plane<3, T>(d.normal, tg::dot(d.normal, d.center - pos<3, T>())));
        if (!r.has_any() || (v.origin + v.dir * r.first() - d.center).length_sqr() > d.radius * d.radius)
            return {};
        return r;
    }
};

// --- aabb and box

template <class L, int D, class T>
    requires tg::impl::is_linear<L>
struct tg::impl::intersection_parameter_op<L, tg::aabb<D, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, aabb<D, T> const& b)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::slabs(v.origin, v.dir, b.min, b.max);
        if (!r.has_value())
            return {};
        return impl::clip(v, r.value().start, r.value().end);
    }
};

/// The slab interval's ends are where the faces are crossed.
template <class L, int D, class T>
    requires tg::impl::is_linear<L>
struct tg::impl::intersection_parameter_op<L, tg::aabb_boundary<D, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, aabb_boundary<D, T> const& b)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::slabs(v.origin, v.dir, b.min, b.max);
        if (!r.has_value())
            return {};
        return impl::crossings<2>(v, r.value().start, r.value().end);
    }
};

template <class L, int D, class T>
    requires(tg::impl::is_linear<L> && !tg::traits::is_exact<T>)
struct tg::impl::intersection_parameter_op<L, tg::box<D, D, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, box<D, D, T> const& b)
    {
        auto const v = impl::into_frame(impl::linear_of(l), b.center, b.half_extents);
        auto const r = impl::slabs(v.origin, v.dir, pos<D, T>(T(-1)), pos<D, T>(T(1)));
        if (!r.has_value())
            return {};
        return impl::clip(v, r.value().start, r.value().end);
    }
};

template <class L, int D, class T>
    requires(tg::impl::is_linear<L> && !tg::traits::is_exact<T>)
struct tg::impl::intersection_parameter_op<L, tg::box_boundary<D, D, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, box_boundary<D, D, T> const& b)
    {
        auto const v = impl::into_frame(impl::linear_of(l), b.center, b.half_extents);
        auto const r = impl::slabs(v.origin, v.dir, pos<D, T>(T(-1)), pos<D, T>(T(1)));
        if (!r.has_value())
            return {};
        return impl::crossings<2>(v, r.value().start, r.value().end);
    }
};

// --- ellipsoid, as the unit ball in its own frame

template <class L, int D, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::ellipsoid<D, D, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, ellipsoid<D, D, T> const& e)
    {
        mat<D, D, T> m;
        for (int i = 0; i < D; ++i)
            m.cols[i] = e.semi_axes[i];
        auto const v = impl::into_frame(impl::linear_of(l), e.center, m);
        auto const r = impl::sphere_roots(v.origin, v.dir, pos<D, T>(), T(1));
        if (!r.has_value())
            return {};
        return impl::clip(v, r.value().start, r.value().end);
    }
};

template <class L, int D, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::ellipsoid_boundary<D, D, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, ellipsoid_boundary<D, D, T> const& e)
    {
        mat<D, D, T> m;
        for (int i = 0; i < D; ++i)
            m.cols[i] = e.semi_axes[i];
        auto const v = impl::into_frame(impl::linear_of(l), e.center, m);
        auto const r = impl::sphere_roots(v.origin, v.dir, pos<D, T>(), T(1));
        if (!r.has_value())
            return {};
        return impl::crossings<2>(v, r.value().start, r.value().end);
    }
};

// --- triangle

/// In 3D a triangle is a patch: one crossing, from the barycentrics of where the line meets its plane.
/// A line lying in the triangle's plane is the special case, and gives non-finite barycentrics, so no hit.
template <class L, class T>
    requires(tg::impl::is_linear<L> && !tg::traits::is_exact<T>)
struct tg::impl::intersection_parameter_op<L, tg::triangle<3, T>>
{
    [[nodiscard]] static constexpr hits<1, T> apply(L const& l, triangle<3, T> const& tri)
    {
        auto const v = impl::linear_of(l);
        auto const e1 = tri.pos1 - tri.pos0;
        auto const e2 = tri.pos2 - tri.pos0;
        auto const p = tg::dual(tg::cross(v.dir, e2));
        auto const det = tg::dot(e1, p);
        TG_SPECIAL_CASE(tg::traits::is_zero(det), "a linear object in a triangle's plane");
        auto const inv = T(1) / det;
        auto const s = v.origin - tri.pos0;
        auto const u = tg::dot(s, p) * inv;
        auto const q = tg::dual(tg::cross(s, e1));
        auto const w = tg::dot(v.dir, q) * inv;
        hits<1, T> r;
        if (!(u >= T(0) && w >= T(0) && u + w <= T(1)))
            return r;
        auto const t = tg::dot(e2, q) * inv;
        if (v.lo <= t && t <= v.hi)
            r.add(t);
        return r;
    }
};

/// In 2D a triangle is a solid: the line clipped against the three edges' inner sides.
template <class L, class T>
    requires(tg::impl::is_linear<L> && !tg::traits::is_exact<T>)
struct tg::impl::intersection_parameter_op<L, tg::triangle<2, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, triangle<2, T> const& tri)
    {
        auto const v = impl::linear_of(l);
        pos<2, T> const p[3] = {tri.pos0, tri.pos1, tri.pos2};
        auto const e01 = p[1] - p[0];
        auto const e02 = p[2] - p[0];
        // counter-clockwise or not, the inside of edge (a, b) is where cross(b - a, x - a) has the winding's sign
        auto const winding = e01.data[0] * e02.data[1] - e01.data[1] * e02.data[0] > T(0) ? T(1) : T(-1);
        auto const inf = impl::unbounded_parameter<T>();
        auto t0 = -inf;
        auto t1 = inf;
        for (int i = 0; i < 3; ++i)
        {
            auto const a = p[i];
            auto const e = p[(i + 1) % 3] - a;
            // side(t) = winding * cross(e, origin + t dir - a) >= 0 is the inside
            auto const s0
                = winding * (e.data[0] * (v.origin.data[1] - a.data[1]) - e.data[1] * (v.origin.data[0] - a.data[0]));
            auto const ds = winding * (e.data[0] * v.dir.data[1] - e.data[1] * v.dir.data[0]);
            if (tg::traits::is_zero(ds))
            {
                if (s0 < T(0))
                    return {};
                continue;
            }
            auto const t = -s0 / ds;
            if (ds > T(0))
                t0 = t > t0 ? t : t0;
            else
                t1 = t < t1 ? t : t1;
        }
        return impl::clip(v, t0, t1);
    }
};

// --- two linear objects in the plane

/// The crossing of two lines in 2D, kept where it falls in both ranges; parameters along the first.
/// Parallel lines are the special case, and give a non-finite parameter, so no hit.
template <class L, class M>
    requires(tg::impl::is_linear<L> && tg::impl::is_linear<M>
             && decltype(tg::impl::linear_of(*static_cast<L const*>(nullptr)))::dim == 2)
struct tg::impl::intersection_parameter_op<L, M>
{
    [[nodiscard]] static constexpr auto apply(L const& l, M const& m)
    {
        auto const a = impl::linear_of(l);
        auto const b = impl::linear_of(m);
        using T = decltype(a.lo);
        auto const cross2 = [](auto const& x, auto const& y) { return x.data[0] * y.data[1] - x.data[1] * y.data[0]; };
        auto const denom = cross2(a.dir, b.dir);
        TG_SPECIAL_CASE(tg::traits::is_zero(denom), "two parallel lines in the plane");
        auto const w = b.origin - a.origin;
        auto const t = cross2(w, b.dir) / denom;
        auto const s = cross2(w, a.dir) / denom;
        hits<1, T> r;
        if (a.lo <= t && t <= a.hi && b.lo <= s && s <= b.hi)
            r.add(t);
        return r;
    }
};
