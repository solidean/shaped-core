#pragma once

#include <typed-geometry/geometry/query/contains.hh>
#include <typed-geometry/geometry/query/impl/kernels.hh>

/// `a.intersects(b)`: a and b share a point.

namespace tg::impl
{
template <class A, class B>
[[nodiscard]] constexpr bool intersects(A const& a, B const& b)
{
    static_assert(written_once<intersects_op, A, B>, "tg: an intersects kernel is written in both orders");

    if constexpr (has_op<intersects_op, A, B>)
        return intersects_op<A, B>::apply(a, b);
    else if constexpr (has_op<intersects_op, B, A>)
        return intersects_op<B, A>::apply(b, a);
    else if constexpr (is_pos<A> && has_contains<B, A>)
        return impl::contains(b, a);
    else if constexpr (is_pos<B> && has_contains<A, B>)
        return impl::contains(a, b);
    else
        static_assert(false, "tg: no intersects for this pair of types; tg::has_intersects<A, B> is the probe");
}
} // namespace tg::impl

// intersects, one definition per object type

template <int D, class T>
template <class Obj>
constexpr auto tg::pos<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::segment<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::ray<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::line<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::triangle<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::plane<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere<D, D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere<2, 3, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere_boundary<D, D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere_boundary<2, 3, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid<D, DAmbient, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid_boundary<D, DAmbient, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}
