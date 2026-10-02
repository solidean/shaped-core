#pragma once

#include <clean-core/container/pair.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/geometry/query/impl/special_case.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/mat.hh>
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
/// The halving is capped because an exact scalar that does not declare `is_exact` never reaches 1 + e/2 == 1.
template <class T>
[[nodiscard]] constexpr T machine_epsilon()
{
    auto e = T(1);
    for (auto i = 0; i < 4096 && T(1) + e / T(2) != T(1); ++i)
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

/// The point of a simplex nearest the origin, as barycentrics over the vertices of the face it lies on.
template <int D, class T>
struct simplex_face
{
    int idx[4] = {};
    T bary[4] = {};
    int k = 0;
};

template <int D, class T>
[[nodiscard]] constexpr T face_distance_sqr(gjk_vertex<D, T> const* s, simplex_face<D, T> const& f)
{
    auto v = vec<D, T>();
    for (int m = 0; m < f.k; ++m)
        v = v + s[f.idx[m]].w * f.bary[m];
    return v.length_sqr();
}

/// Nearest point of the segment s[i] s[j] to the origin.
template <int D, class T>
[[nodiscard]] constexpr simplex_face<D, T> nearest_on_segment(gjk_vertex<D, T> const* s, int i, int j)
{
    auto const a = s[i].w;
    auto const ab = s[j].w - a;
    auto const ab2 = tg::dot(ab, ab);
    auto const t = tg::traits::is_zero(ab2) ? T(1) : -tg::dot(a, ab) / ab2;
    if (t <= T(0))
        return {{i}, {T(1)}, 1};
    if (t >= T(1))
        return {{j}, {T(1)}, 1};
    return {{i, j}, {T(1) - t, t}, 2};
}

/// Nearest point of the triangle s[i] s[j] s[k] to the origin, by the same seven regions the point-triangle
/// projection uses, read off dot products with the two edges at each vertex.
template <int D, class T>
[[nodiscard]] constexpr simplex_face<D, T> nearest_on_triangle(gjk_vertex<D, T> const* s, int i, int j, int k)
{
    auto const a = s[i].w;
    auto const b = s[j].w;
    auto const c = s[k].w;
    auto const ab = b - a;
    auto const ac = c - a;

    auto const d1 = -tg::dot(ab, a);
    auto const d2 = -tg::dot(ac, a);
    if (d1 <= T(0) && d2 <= T(0))
        return {{i}, {T(1)}, 1};

    auto const d3 = -tg::dot(ab, b);
    auto const d4 = -tg::dot(ac, b);
    if (d3 >= T(0) && d4 <= d3)
        return {{j}, {T(1)}, 1};

    auto const vc = d1 * d4 - d3 * d2;
    if (vc <= T(0) && d1 >= T(0) && d3 <= T(0))
    {
        auto const t = d1 / (d1 - d3);
        return {{i, j}, {T(1) - t, t}, 2};
    }

    auto const d5 = -tg::dot(ab, c);
    auto const d6 = -tg::dot(ac, c);
    if (d6 >= T(0) && d5 <= d6)
        return {{k}, {T(1)}, 1};

    auto const vb = d5 * d2 - d1 * d6;
    if (vb <= T(0) && d2 >= T(0) && d6 <= T(0))
    {
        auto const t = d2 / (d2 - d6);
        return {{i, k}, {T(1) - t, t}, 2};
    }

    auto const va = d3 * d6 - d5 * d4;
    if (va <= T(0) && d4 - d3 >= T(0) && d5 - d6 >= T(0))
    {
        auto const t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {{j, k}, {T(1) - t, t}, 2};
    }

    auto const sum = va + vb + vc;
    if (tg::traits::is_zero(sum))
    {
        // a collinear triangle: the nearest of its three edges
        simplex_face<D, T> const edges[3]
            = {impl::nearest_on_segment(s, i, j), impl::nearest_on_segment(s, j, k), impl::nearest_on_segment(s, i, k)};
        auto best = 0;
        for (int e = 1; e < 3; ++e)
            if (impl::face_distance_sqr(s, edges[e]) < impl::face_distance_sqr(s, edges[best]))
                best = e;
        return edges[best];
    }
    return {{i, j, k}, {va / sum, vb / sum, vc / sum}, 3};
}


/// Nearest point of the tetrahedron to the origin: the origin itself when its barycentrics are all non-negative,
/// otherwise the nearest point of the four faces.
/// No orientation test decides between them, because on a nearly flat tetrahedron — two segments' Minkowski
/// difference is a parallelogram — those signs are noise; a volume small against its edges counts as flat.
template <int D, class T>
[[nodiscard]] constexpr simplex_face<D, T> nearest_on_tetrahedron(gjk_vertex<D, T> const* s)
{
    static_assert(D == 3, "only a 3D simplex has four vertices");
    auto const e1 = s[1].w - s[0].w;
    auto const e2 = s[2].w - s[0].w;
    auto const e3 = s[3].w - s[0].w;
    auto const volume = tg::dot(tg::dual(tg::cross(e1, e2)), e3);
    auto const reach = e1.length_sqr() * e2.length_sqr() * e3.length_sqr();
    auto const tol = impl::machine_epsilon<T>() * T(64);

    if (volume * volume > tol * tol * reach)
    {
        // s0 + sum_m l_m (s_m - s0) = 0
        auto const l = mat<3, 3, T>::make_from_cols(e1, e2, e3).inverse() * -s[0].w;
        auto const l0 = T(1) - l.data[0] - l.data[1] - l.data[2];
        if (l0 >= T(0) && l.data[0] >= T(0) && l.data[1] >= T(0) && l.data[2] >= T(0))
            return {{0, 1, 2, 3}, {l0, l.data[0], l.data[1], l.data[2]}, 4};
    }

    int const faces[4][3] = {{1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2}};
    simplex_face<D, T> best = impl::nearest_on_triangle(s, faces[0][0], faces[0][1], faces[0][2]);
    auto best_d2 = impl::face_distance_sqr(s, best);
    for (int f = 1; f < 4; ++f)
    {
        auto const c = impl::nearest_on_triangle(s, faces[f][0], faces[f][1], faces[f][2]);
        auto const d2 = impl::face_distance_sqr(s, c);
        if (d2 < best_d2)
        {
            best = c;
            best_d2 = d2;
        }
    }
    return best;
}

/// Replaces the simplex with the face of it nearest the origin, and writes that point's barycentrics.
template <int D, class T>
constexpr void reduce_simplex(gjk_vertex<D, T>* s, int& n, T* bary)
{
    simplex_face<D, T> f;
    if (n == 1)
        f = {{0}, {T(1)}, 1};
    else if (n == 2)
        f = impl::nearest_on_segment(s, 0, 1);
    else if (n == 3)
        f = impl::nearest_on_triangle(s, 0, 1, 2);
    else if constexpr (D == 3)
        f = impl::nearest_on_tetrahedron(s);

    gjk_vertex<D, T> kept[4] = {};
    for (int m = 0; m < f.k; ++m)
    {
        kept[m] = s[f.idx[m]];
        bary[m] = f.bary[m];
    }
    for (int m = 0; m < f.k; ++m)
        s[m] = kept[m];
    n = f.k;
}

/// The exhaustive form of reduce_simplex: every face of the simplex, the nearest one with positive barycentrics.
/// Slower, and robust where the closed forms' region tests turn noisy — a nearly degenerate face late in a run.
///
/// Every non-empty subset is a face; the nearest point of a face's affine hull comes from a Gram system of at most
/// D x D, and a face qualifies when every barycentric is positive.
/// The nearest qualifying face is the answer, and the vertices outside it are dropped from the simplex.
template <int D, class T>
constexpr void reduce_simplex_exhaustive(gjk_vertex<D, T>* s, int& n, T* bary)
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
        // the whole simplex is never a candidate here: on a nearly flat one its system solves to noise that can pass
        // the positivity test, and enclosing the origin is the closed-form tetrahedron's call, guarded by its volume
        if (k == D + 1)
            continue;

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
        // |v| within the tolerance relative to the largest support point seen
        if (v2 <= tol * tol * scale2)
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
        gjk_vertex<D, T> before[D + 1] = {};
        auto const before_size = r.simplex_size;
        for (auto i = 0; i < before_size; ++i)
            before[i] = r.simplex[i];

        auto const nearest = [&]
        {
            auto p = r.simplex[0].w * bary[0];
            for (auto i = 1; i < r.simplex_size; ++i)
                p = p + r.simplex[i].w * bary[i];
            return p;
        };

        impl::reduce_simplex(r.simplex, r.simplex_size, bary);
        auto nv = nearest();
        // the closed forms stalled: retry the same simplex with the exhaustive search before giving up
        if (!(nv.length_sqr() < v2))
        {
            for (auto i = 0; i < before_size; ++i)
                r.simplex[i] = before[i];
            r.simplex_size = before_size;
            impl::reduce_simplex_exhaustive(r.simplex, r.simplex_size, bary);
            nv = nearest();
        }

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
