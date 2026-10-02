#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace tg::impl
{
/// the smallest aabb holding every point given.
template <int D, class T, class... Ps>
[[nodiscard]] constexpr aabb<D, T> bounds_of(pos<D, T> const& first, Ps const&... rest)
{
    auto r = aabb<D, T>(first, first);
    auto const grow = [&](pos<D, T> const& p)
    {
        for (int i = 0; i < D; ++i)
        {
            if (p.data[i] < r.min.data[i])
                r.min.data[i] = p.data[i];
            if (p.data[i] > r.max.data[i])
                r.max.data[i] = p.data[i];
        }
    };
    (grow(rest), ...);
    return r;
}

/// per coordinate, how far a disk of radius r perpendicular to the unit-free axis u reaches: r * sqrt(1 - u_k^2 / |u|^2).
template <class T>
[[nodiscard]] constexpr vec<3, T> disk_reach(vec<3, T> const& u, T r)
{
    auto const uu = tg::dot(u, u);
    vec<3, T> e;
    for (int k = 0; k < 3; ++k)
    {
        auto const s = T(1) - u.data[k] * u.data[k] / uu;
        e.data[k] = r * tg::sqrt(s > T(0) ? s : T(0));
    }
    return e;
}
} // namespace tg::impl
