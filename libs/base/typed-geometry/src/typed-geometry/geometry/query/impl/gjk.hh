#pragma once

#include <clean-core/container/pair.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/geometry/query/impl/special_case.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>

/// GJK: the closest points of two bounded convex sets, from their support functions alone.
///
/// It walks a simplex of points of the Minkowski difference A - B towards the origin.
/// Each step asks both supports for the point farthest along the current search direction, and replaces the simplex with
/// the face of it nearest the origin.
/// The distance between A and B is the distance from the origin to A - B, and the closest points are read off the
/// barycentrics of the final face.
///
/// The search stops at a relative tolerance built from the scalar's own epsilon, or after a fixed number of steps, in
/// which case the current answer is returned; neither asserts.
/// libs/base/typed-geometry/docs/plans/geometry-query-matrix.md has the contract a scalar must uphold to be used here.

namespace tg::impl
{
/// the smallest e with 1 + e != 1, found by halving, so a scalar needs no epsilon of its own.
template <class T>
[[nodiscard]] constexpr T machine_epsilon()
{
    auto e = T(1);
    while (T(1) + e / T(2) != T(1))
        e = e / T(2);
    return e;
}

template <int D, class T>
struct gjk_vertex
{
    vec<D, T> w; // a - b, a point of the Minkowski difference
    pos<D, T> a;
    pos<D, T> b;
};

template <int D, class T>
struct gjk_result
{
    pos<D, T> on_a;
    pos<D, T> on_b;
    bool overlapping = false;
    /// the final simplex, which EPA continues from when the sets overlap.
    gjk_vertex<D, T> simplex[D + 1] = {};
    int simplex_size = 0;
};

/// The point of the simplex nearest the origin, as barycentrics over a subset of its vertices.
///
/// Every non-empty subset is a face; the nearest point of a face's affine hull comes from a Gram system of at most
/// D x D, and a face qualifies when every barycentric is positive.
/// The nearest qualifying face is the answer, and the vertices outside it are dropped from the simplex.
template <int D, class T>
constexpr void reduce_simplex(gjk_vertex<D, T>* s, int& n, T* bary)
{
    auto best_mask = 0;
    auto best_d2 = T(0);
    T best_bary[D + 1] = {};

    for (auto mask = 1; mask < (1 << n); ++mask)
    {
        int idx[D + 1] = {};
        auto k = 0;
        for (auto i = 0; i < n; ++i)
            if (mask & (1 << i))
                idx[k++] = i;

        // the point is w0 + sum_j l_j (w_j - w0), with the Gram system G l = -(e_j . w0)
        T lambda[D + 1] = {};
        auto ok = true;
        if (k > 1)
        {
            T g[D][D + 1] = {};
            for (auto r = 0; r < k - 1; ++r)
            {
                auto const er = s[idx[r + 1]].w - s[idx[0]].w;
                for (auto c = 0; c < k - 1; ++c)
                    g[r][c] = tg::dot(er, s[idx[c + 1]].w - s[idx[0]].w);
                g[r][k - 1] = -tg::dot(er, s[idx[0]].w);
            }

            // Gaussian elimination with partial pivoting; a singular face is degenerate and skipped
            for (auto c = 0; c < k - 1 && ok; ++c)
            {
                auto piv = c;
                for (auto r = c + 1; r < k - 1; ++r)
                    if ((g[r][c] < T(0) ? -g[r][c] : g[r][c]) > (g[piv][c] < T(0) ? -g[piv][c] : g[piv][c]))
                        piv = r;
                if (tg::traits::is_zero(g[piv][c]))
                {
                    ok = false;
                    break;
                }
                for (auto cc = 0; cc < k; ++cc)
                {
                    auto const t = g[c][cc];
                    g[c][cc] = g[piv][cc];
                    g[piv][cc] = t;
                }
                for (auto r = 0; r < k - 1; ++r)
                    if (r != c)
                    {
                        auto const f = g[r][c] / g[c][c];
                        for (auto cc = c; cc < k; ++cc)
                            g[r][cc] = g[r][cc] - f * g[c][cc];
                    }
            }
            if (!ok)
                continue;

            auto l0 = T(1);
            for (auto j = 0; j < k - 1; ++j)
            {
                lambda[j + 1] = g[j][k - 1] / g[j][j];
                l0 = l0 - lambda[j + 1];
            }
            lambda[0] = l0;
        }
        else
            lambda[0] = T(1);

        for (auto j = 0; j < k; ++j)
            if (!(lambda[j] > T(0)))
                ok = false;
        if (!ok)
            continue;

        auto v = s[idx[0]].w * lambda[0];
        for (auto j = 1; j < k; ++j)
            v = v + s[idx[j]].w * lambda[j];
        auto const d2 = v.length_sqr();
        if (best_mask == 0 || d2 < best_d2)
        {
            best_mask = mask;
            best_d2 = d2;
            for (auto j = 0; j < k; ++j)
                best_bary[j] = lambda[j];
        }
    }

    // nothing qualified only on a fully degenerate simplex: keep the newest vertex alone
    if (best_mask == 0)
    {
        s[0] = s[n - 1];
        n = 1;
        bary[0] = T(1);
        return;
    }

    auto k = 0;
    for (auto i = 0; i < n; ++i)
        if (best_mask & (1 << i))
        {
            s[k] = s[i];
            bary[k] = best_bary[k];
            ++k;
        }
    n = k;
}

template <class A, class B>
[[nodiscard]] constexpr auto gjk(A const& a, B const& b)
{
    constexpr int D = traits::ambient_dim<A>;
    using T = traits::scalar_t<A>;
    static_assert(D == traits::ambient_dim<B>, "tg: GJK needs both objects in the same space");
    static_assert(!tg::traits::is_exact<T>, "tg: GJK iterates to a tolerance, so an exact scalar is refused");

    auto const support = [&](vec<D, T> const& dir)
    {
        auto const pa = support_op<A>::apply(a, dir);
        auto const pb = support_op<B>::apply(b, -dir);
        return gjk_vertex<D, T>{.w = pa - pb, .a = pa, .b = pb};
    };

    auto const eps = machine_epsilon<T>();
    auto const tol = eps * T(64);
    constexpr int max_steps = 64;

    gjk_result<D, T> r;
    T bary[D + 1] = {};

    auto dir0 = vec<D, T>();
    dir0.data[0] = T(1);
    r.simplex[0] = support(dir0);
    r.simplex_size = 1;
    bary[0] = T(1);
    auto v = r.simplex[0].w;
    // the scale overlap is judged against: the largest support point seen, so the test is relative
    auto scale2 = v.length_sqr();

    for (auto step = 0; step < max_steps; ++step)
    {
        auto const v2 = v.length_sqr();
        if (v2 <= tol * scale2)
        {
            r.overlapping = true;
            break;
        }

        auto const w = support(-v);
        if (w.w.length_sqr() > scale2)
            scale2 = w.w.length_sqr();
        // no point of A - B lies farther towards the origin than v does: v is the nearest, up to the tolerance
        if (v2 - tg::dot(v, w.w) <= tol * v2)
            break;

        r.simplex[r.simplex_size++] = w;
        impl::reduce_simplex(r.simplex, r.simplex_size, bary);

        auto nv = r.simplex[0].w * bary[0];
        for (auto i = 1; i < r.simplex_size; ++i)
            nv = nv + r.simplex[i].w * bary[i];

        // a full simplex encloses the origin
        if (r.simplex_size == D + 1)
        {
            r.overlapping = true;
            v = nv;
            break;
        }

        // no progress means the tolerance has been reached from the other side
        if (!(nv.length_sqr() < v2))
        {
            v = nv;
            break;
        }
        v = nv;
    }

    auto on_a = vec<D, T>();
    auto on_b = vec<D, T>();
    for (auto i = 0; i < r.simplex_size; ++i)
    {
        on_a = on_a + (r.simplex[i].a - pos<D, T>()) * bary[i];
        on_b = on_b + (r.simplex[i].b - pos<D, T>()) * bary[i];
    }
    r.on_a = pos<D, T>() + on_a;
    r.on_b = r.overlapping ? r.on_a : pos<D, T>() + on_b;
    return r;
}
} // namespace tg::impl
