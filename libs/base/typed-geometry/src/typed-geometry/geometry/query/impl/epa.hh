#pragma once

#include <clean-core/error/optional.hh>
#include <typed-geometry/geometry/query/impl/gjk.hh>
#include <typed-geometry/linalg/cross.hh>

/// EPA: how deep two overlapping bounded convex solids are, continuing from the simplex GJK ended with.
///
/// GJK stops once its simplex encloses the origin, which says the sets overlap and nothing about how much.
/// EPA grows that simplex into a polytope inside A - B, always pushing out the face nearest the origin by asking the
/// supports for the farthest point along its normal.
/// When the support adds nothing beyond that face, the face is on the boundary of A - B: its distance from the origin is
/// the depth, and its normal the direction b moves out of a.
///
/// 2D and 3D only; both stop at a relative tolerance or a fixed step count, and the latter returns the best face so far.

/// How far, and which way, b has to move to stop overlapping a: by `normal * depth`.
/// `normal` is unit length, and `depth` is never negative.
template <int D, class T>
struct tg::separation
{
    vec<D, T> normal;
    T depth = {};
};

namespace tg::impl
{
/// Completes a simplex that encloses the origin but spans less than the whole space — GJK may stop with one as soon as
/// it gets close enough — by adding support points along the coordinate axes until it is a full simplex.
template <int D, class T, class Support>
constexpr bool complete_simplex(gjk_vertex<D, T>* s, int& n, Support const& support)
{
    for (auto axis = 0; axis < D && n < D + 1; ++axis)
        for (auto sign : {T(1), T(-1)})
        {
            if (n == D + 1)
                break;
            auto dir = vec<D, T>();
            dir.data[axis] = sign;
            auto const w = support(dir);

            // accept w only if it leaves the affine hull of the current simplex
            auto independent = true;
            if constexpr (D == 2)
            {
                if (n == 2)
                {
                    auto const e = s[1].w - s[0].w;
                    auto const f = w.w - s[0].w;
                    independent = !tg::traits::is_zero(e.data[0] * f.data[1] - e.data[1] * f.data[0]);
                }
            }
            else
            {
                if (n == 2)
                {
                    auto const c = tg::dual(tg::cross(s[1].w - s[0].w, w.w - s[0].w));
                    independent = !tg::traits::is_zero(c.length_sqr());
                }
                else if (n == 3)
                {
                    auto const c = tg::dual(tg::cross(s[1].w - s[0].w, s[2].w - s[0].w));
                    independent = !tg::traits::is_zero(tg::dot(c, w.w - s[0].w));
                }
            }
            if (n == 1)
                independent = !tg::traits::is_zero((w.w - s[0].w).length_sqr());

            if (independent)
                s[n++] = w;
        }
    return n == D + 1;
}

template <class A, class B>
[[nodiscard]] constexpr auto epa(A const& a, B const& b, gjk_result<traits::ambient_dim<A>, traits::scalar_t<A>> const& g)
    -> cc::optional<separation<traits::ambient_dim<A>, traits::scalar_t<A>>>
{
    constexpr int D = traits::ambient_dim<A>;
    using T = traits::scalar_t<A>;
    static_assert(D == 2 || D == 3, "tg: EPA is implemented for 2D and 3D");

    if (!g.overlapping)
        return {};

    auto const support = [&](vec<D, T> const& dir)
    {
        auto const pa = support_op<A>::apply(a, dir);
        auto const pb = support_op<B>::apply(b, -dir);
        return gjk_vertex<D, T>{.w = pa - pb, .a = pa, .b = pb};
    };

    gjk_vertex<D, T> simplex[D + 1] = {};
    auto n = g.simplex_size;
    for (auto i = 0; i < n; ++i)
        simplex[i] = g.simplex[i];
    if (!impl::complete_simplex(simplex, n, support))
        return {}; // A - B is flat: two solids that are not full-dimensional, which EPA does not serve

    auto const tol = machine_epsilon<T>() * T(64);
    constexpr int max_steps = 128;
    constexpr int max_vertices = D + 1 + max_steps;

    vec<D, T> verts[max_vertices] = {};
    auto nv = 0;
    for (auto i = 0; i < n; ++i)
        verts[nv++] = simplex[i].w;

    auto scale = T(0);
    for (auto i = 0; i < nv; ++i)
        if (verts[i].length_sqr() > scale)
            scale = verts[i].length_sqr();
    scale = tg::sqrt(scale);

    if constexpr (D == 2)
    {
        // a counter-clockwise polygon; each edge i runs from poly[i] to poly[i + 1]
        int poly[max_vertices] = {0, 1, 2};
        auto np = 3;
        {
            auto const e = verts[1] - verts[0];
            auto const f = verts[2] - verts[0];
            if (e.data[0] * f.data[1] - e.data[1] * f.data[0] < T(0))
            {
                poly[1] = 2;
                poly[2] = 1;
            }
        }

        auto best = separation<D, T>{};
        for (auto step = 0; step < max_steps; ++step)
        {
            auto best_i = 0;
            auto best_d = T(0);
            auto best_n = vec<D, T>();
            for (auto i = 0; i < np; ++i)
            {
                auto const& p = verts[poly[i]];
                auto const& q = verts[poly[(i + 1) % np]];
                auto const e = q - p;
                auto nrm = vec<D, T>(e.data[1], -e.data[0]); // outward for a counter-clockwise polygon
                nrm = nrm / nrm.length();
                auto const d = tg::dot(nrm, p);
                if (i == 0 || d < best_d)
                {
                    best_i = i;
                    best_d = d;
                    best_n = nrm;
                }
            }
            // GJK calls an overlap within its tolerance, so the origin can lie a rounding outside the nearest face
            best = {.normal = best_n, .depth = best_d > T(0) ? best_d : T(0)};

            auto const w = support(best_n);
            if (tg::dot(w.w, best_n) - best_d <= tol * scale || nv == max_vertices)
                break;

            verts[nv] = w.w;
            for (auto j = np; j > best_i + 1; --j)
                poly[j] = poly[j - 1];
            poly[best_i + 1] = nv;
            ++nv;
            ++np;
        }
        return best;
    }
    else
    {
        struct face
        {
            int v[3];
            vec<3, T> normal;
            T dist;
        };
        constexpr int max_faces = 4 + 2 * max_steps * 2;
        face faces[max_faces] = {};
        auto nf = 0;

        auto const make_face = [&](int i0, int i1, int i2)
        {
            auto nrm = tg::dual(tg::cross(verts[i1] - verts[i0], verts[i2] - verts[i0]));
            nrm = nrm / nrm.length();
            return face{.v = {i0, i1, i2}, .normal = nrm, .dist = tg::dot(nrm, verts[i0])};
        };

        // the tetrahedron's faces, each turned to face away from the opposite vertex
        int const tet[4][4] = {{0, 1, 2, 3}, {0, 3, 1, 2}, {0, 2, 3, 1}, {1, 3, 2, 0}};
        for (auto const& t : tet)
        {
            auto f = make_face(t[0], t[1], t[2]);
            if (tg::dot(f.normal, verts[t[3]] - verts[t[0]]) > T(0))
                f = make_face(t[0], t[2], t[1]);
            faces[nf++] = f;
        }

        auto best = separation<D, T>{};
        for (auto step = 0; step < max_steps; ++step)
        {
            auto bi = 0;
            for (auto i = 1; i < nf; ++i)
                if (faces[i].dist < faces[bi].dist)
                    bi = i;
            // GJK calls an overlap within its tolerance, so the origin can lie a rounding outside the nearest face
            best = {.normal = faces[bi].normal, .depth = faces[bi].dist > T(0) ? faces[bi].dist : T(0)};

            auto const w = support(faces[bi].normal);
            if (tg::dot(w.w, faces[bi].normal) - faces[bi].dist <= tol * scale || nv == max_vertices)
                break;

            // remove every face w sees, keeping the boundary of the removed region as the horizon
            int horizon[max_faces * 3][2] = {};
            auto nh = 0;
            auto const toggle_edge = [&](int p, int q)
            {
                for (auto i = 0; i < nh; ++i)
                    if (horizon[i][0] == q && horizon[i][1] == p)
                    {
                        horizon[i][0] = horizon[nh - 1][0];
                        horizon[i][1] = horizon[nh - 1][1];
                        --nh;
                        return;
                    }
                horizon[nh][0] = p;
                horizon[nh][1] = q;
                ++nh;
            };

            auto kept = 0;
            for (auto i = 0; i < nf; ++i)
            {
                auto const& f = faces[i];
                if (tg::dot(f.normal, w.w - verts[f.v[0]]) > T(0))
                {
                    toggle_edge(f.v[0], f.v[1]);
                    toggle_edge(f.v[1], f.v[2]);
                    toggle_edge(f.v[2], f.v[0]);
                }
                else
                    faces[kept++] = f;
            }
            nf = kept;

            if (nf + nh > max_faces)
                break;

            verts[nv] = w.w;
            for (auto i = 0; i < nh; ++i)
                faces[nf++] = make_face(horizon[i][0], horizon[i][1], nv);
            ++nv;
        }
        return best;
    }
}
} // namespace tg::impl
