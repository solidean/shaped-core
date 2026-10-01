#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/linalg/pos.hh>

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
} // namespace tg::impl
