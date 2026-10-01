#pragma once

#include <typed-geometry/geometry/query/impl/kernels.hh>

/// `a.project_to(b)`: a mapped onto b — for a pos, the point of b nearest to it.
/// Only a kernel answers it; there is no derivation and no mirror, since the verb is not symmetric.

namespace tg::impl
{
template <class A, class B>
[[nodiscard]] constexpr auto project_to(A const& a, B const& b)
{
    if constexpr (has_op<project_op, A, B>)
        return project_op<A, B>::apply(a, b);
    else
        static_assert(false, "tg: no project_to for this pair of types; tg::has_project_to<A, B> is the probe");
}
} // namespace tg::impl

// project_to, one definition per object type

template <int D, class T>
template <class Obj>
constexpr auto tg::pos<D, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::segment<D, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::ray<D, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::line<D, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::triangle<D, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::plane<D, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb<D, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere<D, D, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere<2, 3, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere_boundary<D, D, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere_boundary<2, 3, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid<D, DAmbient, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid_boundary<D, DAmbient, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::halfspace<D, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb_boundary<D, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box<D, DAmbient, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box_boundary<D, DAmbient, T>::project_to(Obj const& obj) const
{
    return tg::impl::project_to(*this, obj);
}
