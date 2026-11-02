#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/pair.hh>
#include <typed-geometry/linalg/cross.hh>
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

/// a vector perpendicular to v, of no particular length; the zero vector only for a zero v.
/// In 3D it zeroes x or z, whichever is smaller in magnitude, which keeps the squared result at least half of v's.
template <class T>
[[nodiscard]] constexpr vec<2, T> any_orthogonal(vec<2, T> const& v)
{
    return vec<2, T>(-v.data[1], v.data[0]);
}

template <class T>
[[nodiscard]] constexpr vec<3, T> any_orthogonal(vec<3, T> const& v)
{
    auto const ax = v.data[0] < T(0) ? -v.data[0] : v.data[0];
    auto const az = v.data[2] < T(0) ? -v.data[2] : v.data[2];
    return ax > az ? vec<3, T>(-v.data[1], v.data[0], T(0)) : vec<3, T>(T(0), -v.data[2], v.data[1]);
}

/// two unit vectors that, with the unit vector n, form a right-handed orthonormal basis (u, w, n).
/// n must be unit length; the construction is branch-light and has no singular direction.
template <class T>
[[nodiscard]] constexpr cc::pair<vec<3, T>, vec<3, T>> orthonormal_basis(vec<3, T> const& n)
{
    auto const sign = n.data[2] < T(0) ? T(-1) : T(1);
    auto const a = T(-1) / (sign + n.data[2]);
    auto const b = n.data[0] * n.data[1] * a;
    return {vec<3, T>(T(1) + sign * n.data[0] * n.data[0] * a, sign * b, -sign * n.data[0]),
            vec<3, T>(b, sign + n.data[1] * n.data[1] * a, -n.data[1])};
}

/// The same basis with its roll fixed: w lies in the plane of n and `up`, on up's side, and u = w x n.
/// n must be unit length; `up` need not be unit or perpendicular to n, but must not be parallel to it.
/// A camera frame is this with n = forward (u right, w up), whichever handedness the camera uses.
template <class T>
[[nodiscard]] cc::pair<vec<3, T>, vec<3, T>> orthonormal_basis(vec<3, T> const& n, vec<3, T> const& up)
    requires(tg::traits::has_sqrt<T>)
{
    auto const w = (up - n * dot(up, n)).normalized();
    CC_ASSERT(!tg::traits::is_zero(w.length_sqr()), "orthonormal_basis needs an up vector that is not parallel to n");
    return {tg::dual(tg::cross(w, n)), w};
}
} // namespace tg
