#pragma once

#include <typed-geometry/geometry/primitives/halfspace.hh>
#include <typed-geometry/geometry/query/impl/kernels/plane.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>

/// Kernels against a half-space and its plane.
/// Against any object with a support, both are a comparison of the object's extreme points along the normal.

/// A point inside is its own projection; one outside drops onto the boundary plane.
template <int D, class T>
struct tg::impl::project_op<tg::pos<D, T>, tg::halfspace<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, halfspace<D, T> const& h)
    {
        auto const off = impl::plane_offset(p, h.boundary());
        return off > T(0) ? p - h.normal * off : p;
    }
};

/// Negative inside, the offset from the boundary plane.
template <int D, class T>
struct tg::impl::signed_distance_op<tg::pos<D, T>, tg::halfspace<D, T>>
{
    [[nodiscard]] static constexpr T apply(pos<D, T> const& p, halfspace<D, T> const& h)
    {
        return impl::plane_offset(p, h.boundary());
    }
};

/// The object's lowest point along the normal is inside.
template <int D, class T, class Obj>
    requires(tg::impl::has_support<Obj> && !tg::impl::is_pos<Obj>)
struct tg::impl::intersects_op<tg::halfspace<D, T>, Obj>
{
    [[nodiscard]] static constexpr bool apply(halfspace<D, T> const& h, Obj const& o)
    {
        return impl::plane_offset(support_op<Obj>::apply(o, -h.normal), h.boundary()) <= T(0);
    }
};

/// The object's highest point along the normal is inside.
template <int D, class T, class Obj>
    requires(tg::impl::has_support<Obj>)
struct tg::impl::contains_op<tg::halfspace<D, T>, Obj>
{
    [[nodiscard]] static constexpr bool apply(halfspace<D, T> const& h, Obj const& o)
    {
        return impl::plane_offset(support_op<Obj>::apply(o, h.normal), h.boundary()) <= T(0);
    }
};

/// The object reaches both sides of the plane.
template <int D, class T, class Obj>
    requires(tg::impl::has_support<Obj> && !tg::impl::is_pos<Obj>)
struct tg::impl::intersects_op<tg::plane<D, T>, Obj>
{
    [[nodiscard]] static constexpr bool apply(plane<D, T> const& pl, Obj const& o)
    {
        return impl::plane_offset(support_op<Obj>::apply(o, -pl.normal), pl) <= T(0)
            && impl::plane_offset(support_op<Obj>::apply(o, pl.normal), pl) >= T(0);
    }
};
