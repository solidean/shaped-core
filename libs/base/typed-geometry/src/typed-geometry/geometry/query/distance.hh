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

/// `a.contains(p, eps)` for a point: true when p is in a, false when it is farther than eps from it, either in between.
/// It lives with the distances because the exact distance test is what it defaults to.
template <class A, class B, class T>
[[nodiscard]] constexpr bool contains_within(A const& a, B const& b, T eps)
{
    if constexpr (is_pos<B> && has_distance_sqr_to<A, B>)
        return impl::distance_sqr_to(a, b) <= eps * eps;
    else
        static_assert(false, "tg: contains(b, eps) is defined for a point b with a distance to a");
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

template <int D, class T>
template <class Obj>
constexpr auto tg::halfspace<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb_boundary<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box<D, DAmbient, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box_boundary<D, DAmbient, T>::distance_sqr_to(Obj const& obj) const
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

template <int D, class T>
template <class Obj>
constexpr auto tg::halfspace<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb_boundary<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box<D, DAmbient, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box_boundary<D, DAmbient, T>::distance_to(Obj const& obj) const
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

template <int D, class T>
template <class Obj>
constexpr auto tg::halfspace<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb_boundary<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box<D, DAmbient, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box_boundary<D, DAmbient, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

// contains(b, eps), one definition per object type

template <int D, class T>
template <class Obj>
constexpr bool tg::pos<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::segment<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::ray<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::line<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::triangle<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::plane<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::aabb<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::sphere<D, D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <class T>
template <class Obj>
constexpr bool tg::sphere<2, 3, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::sphere_boundary<D, D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <class T>
template <class Obj>
constexpr bool tg::sphere_boundary<2, 3, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr bool tg::ellipsoid<D, DAmbient, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr bool tg::ellipsoid_boundary<D, DAmbient, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::halfspace<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::aabb_boundary<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr bool tg::box<D, DAmbient, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr bool tg::box_boundary<D, DAmbient, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule_boundary<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_boundary<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_mantle<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule_boundary<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_boundary<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_mantle<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule_boundary<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_boundary<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_mantle<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::capsule<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::capsule_boundary<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::cylinder<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::cylinder_boundary<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::cylinder_mantle<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone_boundary<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone_mantle<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere_boundary<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere_mantle<D, T>::distance_sqr_to(Obj const& obj) const
{
    return tg::impl::distance_sqr_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone_boundary<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone_mantle<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere_boundary<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere_mantle<D, T>::distance_to(Obj const& obj) const
{
    return tg::impl::distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone_boundary<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone_mantle<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere_boundary<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere_mantle<D, T>::signed_distance_to(Obj const& obj) const
{
    return tg::impl::signed_distance_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::cone<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::cone_boundary<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::cone_mantle<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::hemisphere<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::hemisphere_boundary<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::hemisphere_mantle<D, T>::contains(Obj const& obj, T eps) const
{
    return tg::impl::contains_within(*this, obj, eps);
}
