#pragma once

#include <typed-geometry/geometry/query/impl/kernels.hh>
#include <typed-geometry/geometry/query/project.hh>

/// `a.contains(b)`: every point of b is in a.
/// A kernel answers it, or for a pos `b`, whether projecting b onto a leaves it where it is.
/// That derivation is exact wherever the projection returns its input unchanged inside a solid; on a lower-dimensional
/// object it is exact only in exact arithmetic, which is the realtime verbs' contract.

namespace tg::impl
{
template <class A, class B>
[[nodiscard]] constexpr bool contains(A const& a, B const& b)
{
    if constexpr (has_op<contains_op, A, B>)
        return contains_op<A, B>::apply(a, b);
    else if constexpr (is_pos<B> && has_project_to<B, A>)
        return impl::project_to(b, a) == b;
    else
        static_assert(false, "tg: no contains for this pair of types; tg::has_contains<A, B> is the probe");
}
} // namespace tg::impl

// contains, one definition per object type

template <int D, class T>
template <class Obj>
constexpr auto tg::pos<D, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::segment<D, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::ray<D, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::line<D, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::triangle<D, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::plane<D, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb<D, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere<D, D, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere<2, 3, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere_boundary<D, D, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere_boundary<2, 3, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid<D, DAmbient, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid_boundary<D, DAmbient, T>::contains(Obj const& obj) const
{
    return tg::impl::contains(*this, obj);
}
