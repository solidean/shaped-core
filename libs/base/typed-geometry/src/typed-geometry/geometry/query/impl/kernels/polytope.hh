#pragma once

#include <typed-geometry/geometry/primitives/ellipsoid.hh>
#include <typed-geometry/geometry/primitives/quad.hh>
#include <typed-geometry/geometry/primitives/tetrahedron.hh>
#include <typed-geometry/geometry/query/impl/kernels/parameters.hh>
#include <typed-geometry/geometry/query/impl/kernels/triangle.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/geometry/query/impl/special_case.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>

/// Kernels for the tetrahedron and its faces, the bilinear quad, and the ellipsoid's support.

// --- tetrahedron

template <int D, class T>
struct tg::impl::support_op<tg::tetrahedron<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(tetrahedron<D, T> const& t, vec<D, T> const& dir)
    {
        auto const v = t.vertices();
        auto best = 0;
        auto best_d = tg::dot(v[0] - pos<D, T>(), dir);
        for (int i = 1; i < 4; ++i)
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

/// Every barycentric coordinate is non-negative.
template <int D, class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::contains_op<tg::tetrahedron<D, T>, tg::pos<D, T>>
{
    [[nodiscard]] static constexpr bool apply(tetrahedron<D, T> const& t, pos<D, T> const& p)
    {
        auto const b = t.parameter_of(p);
        return b.data[0] >= T(0) && b.data[1] >= T(0) && b.data[2] >= T(0) && b.data[3] >= T(0);
    }
};

/// The point itself inside, otherwise the nearest of the four faces' projections.
template <int D, class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::tetrahedron<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, tetrahedron<D, T> const& t)
    {
        if (contains_op<tetrahedron<D, T>, pos<D, T>>::apply(t, p))
            return p;
        return project_op<pos<D, T>, tetrahedron_boundary<D, T>>::apply(p, t.boundary());
    }
};

template <int D, class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::tetrahedron_boundary<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, tetrahedron_boundary<D, T> const& t)
    {
        auto const faces = t.solid().faces();
        auto best = project_op<pos<D, T>, triangle<D, T>>::apply(p, faces[0]);
        auto best_d2 = (best - p).length_sqr();
        for (int i = 1; i < 4; ++i)
        {
            auto const q = project_op<pos<D, T>, triangle<D, T>>::apply(p, faces[i]);
            auto const d2 = (q - p).length_sqr();
            if (d2 < best_d2)
            {
                best = q;
                best_d2 = d2;
            }
        }
        return best;
    }
};

namespace tg::impl
{
/// the parameters inside the tetrahedron, unclipped: the line clipped against each face's inner side.
template <class T>
[[nodiscard]] constexpr cc::optional<hit_interval<T>> tetrahedron_interval(linear_view<3, T> const& v,
                                                                           tetrahedron<3, T> const& t)
{
    auto const vs = t.vertices();
    auto const inf = impl::unbounded_parameter<T>();
    auto t0 = -inf;
    auto t1 = inf;
    for (int i = 0; i < 4; ++i)
    {
        // the face opposite vertex i, with its normal turned towards that vertex: the inside
        auto const& a = vs[(i + 1) % 4];
        auto const& b = vs[(i + 2) % 4];
        auto const& c = vs[(i + 3) % 4];
        auto n = tg::dual(tg::cross(b - a, c - a));
        if (tg::dot(n, vs[i] - a) < T(0))
            n = -n;
        auto const s0 = tg::dot(n, v.origin - a);
        auto const ds = tg::dot(n, v.dir);
        if (tg::traits::is_zero(ds))
        {
            if (s0 < T(0))
                return {};
            continue;
        }
        auto const tt = -s0 / ds;
        if (ds > T(0))
            t0 = tt > t0 ? tt : t0;
        else
            t1 = tt < t1 ? tt : t1;
    }
    if (!(t0 <= t1))
        return {};
    return hit_interval<T>{.start = t0, .end = t1};
}
} // namespace tg::impl

template <class L, class T>
    requires(tg::impl::is_linear<L> && !tg::traits::is_exact<T>)
struct tg::impl::intersection_parameter_op<L, tg::tetrahedron<3, T>>
{
    [[nodiscard]] static constexpr cc::optional<hit_interval<T>> apply(L const& l, tetrahedron<3, T> const& t)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::tetrahedron_interval(v, t);
        if (!r.has_value())
            return {};
        return impl::clip(v, r.value().start, r.value().end);
    }
};

template <class L, class T>
    requires(tg::impl::is_linear<L> && !tg::traits::is_exact<T>)
struct tg::impl::intersection_parameter_op<L, tg::tetrahedron_boundary<3, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, tetrahedron_boundary<3, T> const& t)
    {
        auto const v = impl::linear_of(l);
        auto const r = impl::tetrahedron_interval(v, t.solid());
        if (!r.has_value())
            return {};
        return impl::crossings<2>(v, r.value().start, r.value().end);
    }
};

// --- quad

/// A line crosses a bilinear patch where P(u, v) - o is parallel to the direction.
/// Two directions perpendicular to it turn that into two bilinear equations in (u, v); eliminating u leaves a
/// quadratic in v, and each root in [0, 1] whose u is also in [0, 1] is a crossing, sorted along the line.
template <class L, class T>
    requires(tg::impl::is_linear<L> && tg::traits::has_sqrt<T>)
struct tg::impl::intersection_parameter_op<L, tg::quad<3, T>>
{
    [[nodiscard]] static constexpr hits<2, T> apply(L const& l, quad<3, T> const& q)
    {
        auto const v = impl::linear_of(l);
        auto const a = q.pos10 - q.pos00;
        auto const b = q.pos01 - q.pos00;
        auto const c = (q.pos11 - q.pos10) - (q.pos01 - q.pos00);
        auto const r = q.pos00 - v.origin;
        auto const e1 = tg::any_orthogonal(v.dir);
        auto const e2 = tg::dual(tg::cross(v.dir, e1));

        // equation i: A_i + B_i u + C_i v + E_i u v = 0
        T const A[2] = {tg::dot(r, e1), tg::dot(r, e2)};
        T const B[2] = {tg::dot(a, e1), tg::dot(a, e2)};
        T const C[2] = {tg::dot(b, e1), tg::dot(b, e2)};
        T const E[2] = {tg::dot(c, e1), tg::dot(c, e2)};

        // (A2 + C2 v)(B1 + E1 v) - (B2 + E2 v)(A1 + C1 v) = 0, as k2 v^2 + k1 v + k0
        auto const k2 = C[1] * E[0] - E[1] * C[0];
        auto const k1 = A[1] * E[0] + C[1] * B[0] - B[1] * C[0] - E[1] * A[0];
        auto const k0 = A[1] * B[0] - B[1] * A[0];

        T roots[2] = {};
        auto n = 0;
        if (tg::traits::is_zero(k2))
        {
            TG_SPECIAL_CASE(tg::traits::is_zero(k1), "a line in the plane of a flat quad");
            roots[n++] = -k0 / k1;
        }
        else
        {
            auto const disc = k1 * k1 - T(4) * k2 * k0;
            if (disc >= T(0))
            {
                auto const s = tg::sqrt(disc);
                roots[n++] = (-k1 - s) / (T(2) * k2);
                roots[n++] = (-k1 + s) / (T(2) * k2);
            }
        }

        T ts[2] = {};
        auto m = 0;
        for (int i = 0; i < n; ++i)
        {
            auto const vv = roots[i];
            if (!(vv >= T(0) && vv <= T(1)))
                continue;
            // u from whichever equation is better conditioned at this v
            auto const d0 = B[0] + E[0] * vv;
            auto const d1 = B[1] + E[1] * vv;
            auto const u = tg::abs(d0) >= tg::abs(d1) ? -(A[0] + C[0] * vv) / d0 : -(A[1] + C[1] * vv) / d1;
            if (!(u >= T(0) && u <= T(1)))
                continue;
            auto const t = tg::dot(q.at(comp<2, T>(u, vv)) - v.origin, v.dir) / tg::dot(v.dir, v.dir);
            if (v.lo <= t && t <= v.hi)
                ts[m++] = t;
        }
        hits<2, T> out;
        if (m == 2 && ts[1] < ts[0])
        {
            auto const t = ts[0];
            ts[0] = ts[1];
            ts[1] = t;
        }
        for (int i = 0; i < m; ++i)
            out.add(ts[i]);
        return out;
    }
};

// --- ellipsoid

/// The semi-axis frame's ball support, mapped: center + M M^T d / |M^T d|, with M the semi-axes as columns.
template <int D, int DAmbient, class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::support_op<tg::ellipsoid<D, DAmbient, T>>
{
    [[nodiscard]] static constexpr pos<DAmbient, T> apply(ellipsoid<D, DAmbient, T> const& e, vec<DAmbient, T> const& dir)
    {
        T m_t_d[D] = {};
        auto len2 = T(0);
        for (int i = 0; i < D; ++i)
        {
            m_t_d[i] = tg::dot(e.semi_axes[i], dir);
            len2 = len2 + m_t_d[i] * m_t_d[i];
        }
        if (tg::traits::is_zero(len2))
            return e.center;
        auto const inv = T(1) / tg::sqrt(len2);
        auto p = e.center;
        for (int i = 0; i < D; ++i)
            p = p + e.semi_axes[i] * (m_t_d[i] * inv);
        return p;
    }
};

/// The point in the semi-axis frame lies in the unit ball.
template <int D, class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::contains_op<tg::ellipsoid<D, D, T>, tg::pos<D, T>>
{
    [[nodiscard]] static constexpr bool apply(ellipsoid<D, D, T> const& e, pos<D, T> const& p)
    {
        mat<D, D, T> m;
        for (int i = 0; i < D; ++i)
            m.cols[i] = e.semi_axes[i];
        return (m.inverse() * (p - e.center)).length_sqr() <= T(1);
    }
};
