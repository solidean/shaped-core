#pragma once

#include <typed-geometry/geometry/primitives/box.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/linalg/vec_ops.hh>

/// Kernels against an oriented box.
/// Its half-axes need not be orthogonal, so clamping in box coordinates is not the nearest point, and there is no
/// projection kernel: distances to a box come from GJK.

/// Each half-axis signed towards the direction.
template <int D, int DAmbient, class T>
struct tg::impl::support_op<tg::box<D, DAmbient, T>>
{
    [[nodiscard]] static constexpr pos<DAmbient, T> apply(box<D, DAmbient, T> const& b, vec<DAmbient, T> const& dir)
    {
        auto r = b.center;
        for (int i = 0; i < D; ++i)
        {
            auto const& axis = b.half_extents.cols[i];
            r = tg::dot(axis, dir) > T(0) ? r + axis : r - axis;
        }
        return r;
    }
};

/// The point in box coordinates lies in [-1, 1] on every axis; exact for any parallelepiped.
template <int D, class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::contains_op<tg::box<D, D, T>, tg::pos<D, T>>
{
    [[nodiscard]] static constexpr bool apply(box<D, D, T> const& b, pos<D, T> const& p)
    {
        auto const c = b.half_extents.inverse() * (p - b.center);
        for (int i = 0; i < D; ++i)
            if (c.data[i] < T(-1) || c.data[i] > T(1))
                return false;
        return true;
    }
};
