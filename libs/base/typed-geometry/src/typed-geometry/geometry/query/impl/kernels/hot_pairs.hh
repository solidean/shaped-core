#pragma once

#include <clean-core/container/pair.hh>
#include <clean-core/error/optional.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/box.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/primitives/sphere.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/geometry/query/impl/special_case.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>

/// Closed forms for the pairs a realtime caller hits most, each a fast path over the GJK floor it would otherwise take.
/// GJK is their test oracle (tests/geometry/query/hot-pairs-test.cc), and the benchmarks beside it say what each buys.

namespace tg::impl
{
template <class T>
[[nodiscard]] constexpr T clamp01(T t)
{
    return t < T(0) ? T(0) : (t > T(1) ? T(1) : t);
}
} // namespace tg::impl

// --- segment and segment

/// The nearest parameters of two segments: the unconstrained minimum of the two lines, clamped to the first segment,
/// the second's matched to it, and the first re-clamped where the second had to clamp.
/// Parallel segments are handled by fixing the first parameter at 0, which is a nearest pair too; a zero-length segment
/// is the special case.
template <int D, class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::closest_points_op<tg::segment<D, T>, tg::segment<D, T>>
{
    [[nodiscard]] static constexpr cc::pair<pos<D, T>, pos<D, T>> apply(segment<D, T> const& p, segment<D, T> const& q)
    {
        auto const d1 = p.pos1 - p.pos0;
        auto const d2 = q.pos1 - q.pos0;
        auto const r = p.pos0 - q.pos0;
        auto const a = tg::dot(d1, d1);
        auto const e = tg::dot(d2, d2);
        TG_SPECIAL_CASE(tg::traits::is_zero(a) || tg::traits::is_zero(e), "a zero-length segment");
        auto const b = tg::dot(d1, d2);
        auto const c = tg::dot(d1, r);
        auto const f = tg::dot(d2, r);
        auto const denom = a * e - b * b;

        auto s = tg::traits::is_zero(denom) ? T(0) : impl::clamp01((b * f - c * e) / denom);
        auto t = (b * s + f) / e;
        if (t < T(0))
        {
            t = T(0);
            s = impl::clamp01(-c / a);
        }
        else if (t > T(1))
        {
            t = T(1);
            s = impl::clamp01((b - c) / a);
        }
        return {p.pos0 + d1 * s, q.pos0 + d2 * t};
    }
};

// --- ball and ball

template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::distance_sqr_op<tg::sphere<D, D, T>, tg::sphere<D, D, T>>
{
    [[nodiscard]] static constexpr T apply(sphere<D, D, T> const& a, sphere<D, D, T> const& b)
    {
        auto const gap = (b.center - a.center).length() - a.radius - b.radius;
        return gap > T(0) ? gap * gap : T(0);
    }
};

template <int D, class T>
struct tg::impl::intersects_op<tg::sphere<D, D, T>, tg::sphere<D, D, T>>
{
    [[nodiscard]] static constexpr bool apply(sphere<D, D, T> const& a, sphere<D, D, T> const& b)
    {
        auto const r = a.radius + b.radius;
        return (b.center - a.center).length_sqr() <= r * r;
    }
};

/// Along the line of centers; concentric balls are the special case, and give a non-finite normal.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::separation_op<tg::sphere<D, D, T>, tg::sphere<D, D, T>>
{
    [[nodiscard]] static constexpr cc::optional<separation<D, T>> apply(sphere<D, D, T> const& a, sphere<D, D, T> const& b)
    {
        auto const between = b.center - a.center;
        auto const d = between.length();
        auto const depth = a.radius + b.radius - d;
        if (depth < T(0))
            return {};
        TG_SPECIAL_CASE(tg::traits::is_zero(d), "two concentric balls");
        return separation<D, T>{.normal = between / d, .depth = depth};
    }
};

// --- aabb and aabb

template <int D, class T>
struct tg::impl::intersects_op<tg::aabb<D, T>, tg::aabb<D, T>>
{
    [[nodiscard]] static constexpr bool apply(aabb<D, T> const& a, aabb<D, T> const& b)
    {
        for (int i = 0; i < D; ++i)
            if (a.max.data[i] < b.min.data[i] || b.max.data[i] < a.min.data[i])
                return false;
        return true;
    }
};

/// The squared gaps between the boxes, axis by axis.
template <int D, class T>
struct tg::impl::distance_sqr_op<tg::aabb<D, T>, tg::aabb<D, T>>
{
    [[nodiscard]] static constexpr T apply(aabb<D, T> const& a, aabb<D, T> const& b)
    {
        auto r = T(0);
        for (int i = 0; i < D; ++i)
        {
            auto const gap = a.min.data[i] > b.max.data[i] ? a.min.data[i] - b.max.data[i]
                           : b.min.data[i] > a.max.data[i] ? b.min.data[i] - a.max.data[i]
                                                           : T(0);
            r = r + gap * gap;
        }
        return r;
    }
};

/// The axis of least overlap, pushing b out on the side its center lies.
template <int D, class T>
struct tg::impl::separation_op<tg::aabb<D, T>, tg::aabb<D, T>>
{
    [[nodiscard]] static constexpr cc::optional<separation<D, T>> apply(aabb<D, T> const& a, aabb<D, T> const& b)
    {
        auto best_axis = -1;
        auto best = T(0);
        auto positive = true;
        for (int i = 0; i < D; ++i)
        {
            auto const up = a.max.data[i] - b.min.data[i];   // move b along +i by this
            auto const down = b.max.data[i] - a.min.data[i]; // or along -i by this
            if (up < T(0) || down < T(0))
                return {};
            auto const along = up < down ? up : down;
            if (best_axis < 0 || along < best)
            {
                best_axis = i;
                best = along;
                positive = up < down;
            }
        }
        auto n = vec<D, T>();
        n.data[best_axis] = positive ? T(1) : T(-1);
        return separation<D, T>{.normal = n, .depth = best};
    }
};

// --- ball and aabb

template <int D, class T>
struct tg::impl::intersects_op<tg::sphere<D, D, T>, tg::aabb<D, T>>
{
    [[nodiscard]] static constexpr bool apply(sphere<D, D, T> const& s, aabb<D, T> const& b)
    {
        auto d2 = T(0);
        for (int i = 0; i < D; ++i)
        {
            auto const c = s.center.data[i];
            auto const gap = c < b.min.data[i] ? b.min.data[i] - c : c > b.max.data[i] ? c - b.max.data[i] : T(0);
            d2 = d2 + gap * gap;
        }
        return d2 <= s.radius * s.radius;
    }
};

/// The box's nearest point to the center, and the ball's toward it; one shared point when they overlap.
template <int D, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::closest_points_op<tg::sphere<D, D, T>, tg::aabb<D, T>>
{
    [[nodiscard]] static constexpr cc::pair<pos<D, T>, pos<D, T>> apply(sphere<D, D, T> const& s, aabb<D, T> const& b)
    {
        auto q = s.center;
        for (int i = 0; i < D; ++i)
            q.data[i]
                = q.data[i] < b.min.data[i] ? b.min.data[i] : (q.data[i] > b.max.data[i] ? b.max.data[i] : q.data[i]);
        auto const v = q - s.center;
        auto const l2 = v.length_sqr();
        if (l2 <= s.radius * s.radius)
            return {q, q};
        return {s.center + v * (s.radius / tg::sqrt(l2)), q};
    }
};

// --- box and box

/// The separating axis theorem over a parallelepiped's face normals and, in 3D, the cross products of the edges.
/// An axis whose radii and center offset all vanish (parallel edges) separates nothing, so it needs no special care.
template <int D, class T>
    requires((D == 2 || D == 3) && !tg::traits::is_exact<T>)
struct tg::impl::intersects_op<tg::box<D, D, T>, tg::box<D, D, T>>
{
    [[nodiscard]] static constexpr bool apply(box<D, D, T> const& a, box<D, D, T> const& b)
    {
        auto const between = b.center - a.center;
        auto const separates = [&](vec<D, T> const& axis)
        {
            auto ra = T(0);
            auto rb = T(0);
            for (int i = 0; i < D; ++i)
            {
                ra = ra + tg::abs(tg::dot(a.half_extents.cols[i], axis));
                rb = rb + tg::abs(tg::dot(b.half_extents.cols[i], axis));
            }
            return tg::abs(tg::dot(between, axis)) > ra + rb;
        };

        if constexpr (D == 2)
        {
            for (auto const* bx : {&a, &b})
                for (int i = 0; i < 2; ++i)
                {
                    auto const h = bx->half_extents.cols[i];
                    if (separates(vec<2, T>(-h.data[1], h.data[0])))
                        return false;
                }
            return true;
        }
        else
        {
            auto const& ha = a.half_extents.cols;
            auto const& hb = b.half_extents.cols;
            auto const n = [](vec<3, T> const& x, vec<3, T> const& y) { return tg::dual(tg::cross(x, y)); };
            vec<3, T> const faces[6] = {n(ha[1], ha[2]), n(ha[2], ha[0]), n(ha[0], ha[1]),
                                        n(hb[1], hb[2]), n(hb[2], hb[0]), n(hb[0], hb[1])};
            for (auto const& axis : faces)
                if (separates(axis))
                    return false;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    if (separates(n(ha[i], hb[j])))
                        return false;
            return true;
        }
    }
};
