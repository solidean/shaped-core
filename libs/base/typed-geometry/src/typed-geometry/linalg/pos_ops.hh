#pragma once

#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/scalar/scalar.hh>

/// Additional pos operations that are naturally free functions.

namespace tg
{
/// squared distance between two points (no square root, available for all scalar types).
template <int D, class T>
[[nodiscard]] constexpr T distance_sqr(pos<D, T> const& a, pos<D, T> const& b)
{
    return (a - b).length_sqr();
}

/// euclidean distance between two points; only for scalar types that support sqrt.
template <int D, class T>
[[nodiscard]] T distance(pos<D, T> const& a, pos<D, T> const& b)
    requires(tg::traits::has_sqrt<T>)
{
    return (a - b).length();
}
} // namespace tg
