#pragma once

#include <typed-geometry/geometry/primitives/plane.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/linalg/vec_ops.hh>

/// Kernels against a plane, whose normal is unit length by the plane's own contract.

namespace tg::impl
{
/// dot(normal, p) - dist: how far p lies on the normal's side of the plane.
template <int D, class T>
[[nodiscard]] constexpr T plane_offset(pos<D, T> const& p, plane<D, T> const& pl)
{
    return tg::dot(pl.normal, p - pos<D, T>()) - pl.dist;
}
} // namespace tg::impl

template <int D, class T>
struct tg::impl::project_op<tg::pos<D, T>, tg::plane<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, plane<D, T> const& pl)
    {
        return p - pl.normal * impl::plane_offset(p, pl);
    }
};

/// Positive on the side the normal points to.
template <int D, class T>
struct tg::impl::signed_distance_op<tg::pos<D, T>, tg::plane<D, T>>
{
    [[nodiscard]] static constexpr T apply(pos<D, T> const& p, plane<D, T> const& pl)
    {
        return impl::plane_offset(p, pl);
    }
};
