#pragma once

#include <typed-geometry/geometry/primitives/line.hh>
#include <typed-geometry/geometry/primitives/ray.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/geometry/query/impl/special_case.hh>
#include <typed-geometry/linalg/vec_ops.hh>

/// Projections onto the linear objects: the parameter of the foot of the perpendicular, clamped to the object.
/// They divide, so an exact scalar has none: a projection's coordinates are rational in the input.

namespace tg::impl
{
/// the parameter t of the foot of the perpendicular from p onto origin + t*dir.
/// A zero dir is the special case, and gives a non-finite t.
template <int D, class T>
[[nodiscard]] constexpr T foot_parameter(pos<D, T> const& p, pos<D, T> const& origin, vec<D, T> const& dir)
{
    auto const dd = tg::dot(dir, dir);
    TG_SPECIAL_CASE(tg::traits::is_zero(dd), "projection onto a linear object with a zero direction");
    return tg::dot(p - origin, dir) / dd;
}
} // namespace tg::impl

template <int D, class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::line<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, line<D, T> const& l)
    {
        return l.origin + l.dir * impl::foot_parameter(p, l.origin, l.dir);
    }
};

template <int D, class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::ray<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, ray<D, T> const& r)
    {
        auto const t = impl::foot_parameter(p, r.origin, r.dir);
        return t > T(0) ? r.origin + r.dir * t : r.origin;
    }
};

template <int D, class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::project_op<tg::pos<D, T>, tg::segment<D, T>>
{
    [[nodiscard]] static constexpr pos<D, T> apply(pos<D, T> const& p, segment<D, T> const& s)
    {
        auto const t = impl::foot_parameter(p, s.pos0, s.pos1 - s.pos0);
        if (t <= T(0))
            return s.pos0;
        if (t >= T(1))
            return s.pos1;
        return s.pos0 + (s.pos1 - s.pos0) * t;
    }
};
