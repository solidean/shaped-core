#pragma once

#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/linalg/pos.hh>

/// Kernels between two points.

/// A point projected onto a point is that point.
template <int D, class T>
struct tg::impl::project_op<tg::pos<D, T>, tg::pos<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const&, pos<D, T> const& q) { return q; }
};
