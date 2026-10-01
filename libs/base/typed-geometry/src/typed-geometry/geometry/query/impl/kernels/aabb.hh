#pragma once

#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>

/// Kernels against an axis-aligned box, which need nothing but comparisons and so serve every scalar.

/// Clamping each coordinate is the nearest point of the box, and the point itself when it is inside.
template <int D, class T>
struct tg::impl::project_op<tg::pos<D, T>, tg::aabb<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, aabb<D, T> const& b)
    {
        auto r = p;
        for (int i = 0; i < D; ++i)
        {
            if (r.data[i] < b.min.data[i])
                r.data[i] = b.min.data[i];
            else if (r.data[i] > b.max.data[i])
                r.data[i] = b.max.data[i];
        }
        return r;
    }
};
