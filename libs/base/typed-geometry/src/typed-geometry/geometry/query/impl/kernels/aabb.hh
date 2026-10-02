#pragma once

#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/linalg/vec_ops.hh>

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

/// Per axis, the max corner where the direction is positive and the min corner otherwise.
template <int D, class T>
struct tg::impl::support_op<tg::aabb<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(aabb<D, T> const& b, vec<D, T> const& dir)
    {
        pos<D, T> r;
        for (int i = 0; i < D; ++i)
            r.data[i] = dir.data[i] > T(0) ? b.max.data[i] : b.min.data[i];
        return r;
    }
};

/// Outside the box, the solid's clamp; inside, the nearest face pulls the point onto itself.
template <int D, class T>
struct tg::impl::project_op<tg::pos<D, T>, tg::aabb_boundary<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, aabb_boundary<D, T> const& b)
    {
        auto const clamped = project_op<pos<D, T>, aabb<D, T>>::apply(p, b.solid());
        if (clamped != p)
            return clamped;

        auto r = p;
        auto best_axis = 0;
        auto best_to_max = false;
        auto best = p.data[0] - b.min.data[0];
        for (int i = 0; i < D; ++i)
        {
            auto const to_min = p.data[i] - b.min.data[i];
            auto const to_max = b.max.data[i] - p.data[i];
            if (to_min < best)
            {
                best = to_min;
                best_axis = i;
                best_to_max = false;
            }
            if (to_max < best)
            {
                best = to_max;
                best_axis = i;
                best_to_max = true;
            }
        }
        r.data[best_axis] = best_to_max ? b.max.data[best_axis] : b.min.data[best_axis];
        return r;
    }
};
