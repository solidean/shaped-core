#pragma once

#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/scalar/scalar.hh>

/// Additional vec operations that are naturally free functions (symmetric / cross-cutting),
/// kept out of vec.hh which holds only the type and its intrinsic members.

namespace tg
{
/// dot product of two vectors.
template <int D, class T>
[[nodiscard]] constexpr T dot(vec<D, T> const& a, vec<D, T> const& b)
{
    T s = a.data[0] * b.data[0];
    for (int i = 1; i < D; ++i)
        s += a.data[i] * b.data[i];
    return s;
}

/// free-function form of vec::normalized(); only for scalar types that support sqrt.
template <int D, class T>
[[nodiscard]] vec<D, T> normalize(vec<D, T> const& v)
    requires(tg::traits::has_sqrt<T>)
{
    return v.normalized();
}

// NOTE: cross(vec3, vec3) -> bivec3 is planned but needs the bivec type
// (see libs/base/typed-geometry/docs/structure.md).
} // namespace tg
