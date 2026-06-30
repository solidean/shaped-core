#pragma once

#include <typed-geometry/linalg/bivec.hh>
#include <typed-geometry/linalg/vec.hh>

/// The wedge product and the 3D Hodge-dual escape hatches between bivectors and vectors.

namespace tg
{
/// wedge product of two 3D vectors, a bivector.
///
/// bivec3 components are stored in the basis order {yz, zx, xy} — chosen precisely so the Hodge
/// dual is the identity cast (e_yz->e_x, e_zx->e_y, e_xy->e_z). With this order each component
/// equals the matching component of the classic cross product, so dual(cross(a, b)) == a x b.
template <class T>
[[nodiscard]] constexpr bivec<3, T> cross(vec<3, T> const& a, vec<3, T> const& b)
{
    bivec<3, T> r;
    r.data[0] = a.data[1] * b.data[2] - a.data[2] * b.data[1]; // yz
    r.data[1] = a.data[2] * b.data[0] - a.data[0] * b.data[2]; // zx
    r.data[2] = a.data[0] * b.data[1] - a.data[1] * b.data[0]; // xy
    return r;
}

/// Hodge dual of a 3D bivector: the pseudovector escape hatch.
/// With the {yz, zx, xy} storage order this is a straight component copy.
template <class T>
[[nodiscard]] constexpr vec<3, T> dual(bivec<3, T> const& b)
{
    vec<3, T> r;
    r.data[0] = b.data[0]; // x <- yz
    r.data[1] = b.data[1]; // y <- zx
    r.data[2] = b.data[2]; // z <- xy
    return r;
}

/// inverse of dual(): turns a 3D pseudovector back into a bivector (also a straight copy).
template <class T>
[[nodiscard]] constexpr bivec<3, T> undual(vec<3, T> const& v)
{
    bivec<3, T> r;
    r.data[0] = v.data[0]; // yz <- x
    r.data[1] = v.data[1]; // zx <- y
    r.data[2] = v.data[2]; // xy <- z
    return r;
}
} // namespace tg
