#pragma once

#include <typed-geometry/geometry/query/closest_points.hh>
#include <typed-geometry/geometry/query/impl/kernels.hh>
#include <typed-geometry/scalar/scalar.hh>

#include <type_traits>

/// `a.distance_sqr_to(b)`, `a.distance_to(b)` and `a.signed_distance_to(b)`.
/// The squared distance is a kernel where a closed form is cheaper, and the length between the closest points otherwise.

namespace tg::impl
{
template <class A, class B>
[[nodiscard]] constexpr auto distance_sqr_to(A const& a, B const& b)
{
    static_assert(written_once<distance_sqr_op, A, B>, "tg: a distance_sqr kernel is written in both orders");

    if constexpr (has_op<distance_sqr_op, A, B>)
        return distance_sqr_op<A, B>::apply(a, b);
    else if constexpr (has_op<distance_sqr_op, B, A>)
        return distance_sqr_op<B, A>::apply(b, a);
    else if constexpr (has_closest_points_to<A, B>)
    {
        auto const [pa, pb] = impl::closest_points_to(a, b);
        return (pb - pa).length_sqr();
    }
    else
        static_assert(false, "tg: no distance_sqr_to for this pair of types; tg::has_distance_sqr_to<A, B> is the "
                             "probe");
}

template <class A, class B>
[[nodiscard]] constexpr auto distance_to(A const& a, B const& b)
{
    auto const d2 = impl::distance_sqr_to(a, b);
    static_assert(tg::traits::has_sqrt<std::remove_cvref_t<decltype(d2)>>, "tg: distance_to needs a scalar with sqrt; "
                                                                           "distance_sqr_to works for every scalar");
    return tg::sqrt(d2);
}

template <class A, class B>
[[nodiscard]] constexpr auto signed_distance_to(A const& a, B const& b)
{
    if constexpr (has_op<signed_distance_op, A, B>)
        return signed_distance_op<A, B>::apply(a, b);
    else
        static_assert(false, "tg: no signed_distance_to for this pair of types; tg::has_signed_distance_to<A, B> is "
                             "the probe");
}
} // namespace tg::impl

// distance_sqr_to, one definition per object type

template <int D, class T>
template <class Obj>
constexpr auto tg::pos<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::segment<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::ray<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::line<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::triangle<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::plane<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere<D, D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere<2, 3, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere_boundary<D, D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere_boundary<2, 3, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid<D, DAmbient, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid_boundary<D, DAmbient, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

// distance_to, one definition per object type

template <int D, class T>
template <class Obj>
constexpr auto tg::pos<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::segment<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::ray<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::line<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::triangle<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::plane<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere<D, D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere<2, 3, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere_boundary<D, D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere_boundary<2, 3, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid<D, DAmbient, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid_boundary<D, DAmbient, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

// signed_distance_to, one definition per object type

template <int D, class T>
template <class Obj>
constexpr auto tg::pos<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::segment<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::ray<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::line<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::triangle<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::plane<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere<D, D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere<2, 3, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere_boundary<D, D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere_boundary<2, 3, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid<D, DAmbient, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid_boundary<D, DAmbient, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}
