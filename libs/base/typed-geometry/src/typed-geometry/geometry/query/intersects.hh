#pragma once

#include <typed-geometry/geometry/query/contains.hh>
#include <typed-geometry/geometry/query/distance.hh>
#include <typed-geometry/geometry/query/impl/gjk.hh>
#include <typed-geometry/geometry/query/impl/kernels.hh>

/// `a.intersects(b)`: a and b share a point.

namespace tg::impl
{
/// whether a parameter result holds anything: hits, or an interval.
template <class R>
[[nodiscard]] constexpr bool any_parameter(R const& r)
{
    if constexpr (requires { r.has_any(); })
        return r.has_any();
    else
        return r.has_value();
}

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
    else if constexpr (is_linear<A> && has_op<intersection_parameter_op, A, B>)
        return impl::any_parameter(intersection_parameter_op<A, B>::apply(a, b));
    else if constexpr (is_linear<B> && has_op<intersection_parameter_op, B, A>)
        return impl::any_parameter(intersection_parameter_op<B, A>::apply(b, a));
    else if constexpr (gjk_pair<A, B>)
        return impl::gjk(a, b).overlapping;
    // a boundary meets b exactly when its solid meets b without swallowing it whole
    else if constexpr (is_boundary<A> && has_intersects_direct<solid_t<A>, B> && has_contains<solid_t<A>, B>)
        return impl::intersects(a.solid(), b) && !impl::contains(a.solid(), b);
    else if constexpr (is_boundary<B> && has_intersects_direct<solid_t<B>, A> && has_contains<solid_t<B>, A>)
        return impl::intersects(b.solid(), a) && !impl::contains(b.solid(), a);
    else
        static_assert(false, "tg: no intersects for this pair of types; tg::has_intersects<A, B> is the probe");
}

/// `a.intersects(b, eps)`: true when a and b meet, false when they are farther apart than eps, either in between.
/// The bracket holds up to rounding; the exact distance test satisfies it, and is the default.
template <class A, class B, class T>
[[nodiscard]] constexpr bool intersects_within(A const& a, B const& b, T eps)
{
    if constexpr (has_distance_sqr_to<A, B>)
        return impl::distance_sqr_to(a, b) <= eps * eps;
    else
        static_assert(false, "tg: no intersects(b, eps) for this pair of types; it needs tg::has_distance_sqr_to<A, "
                             "B>");
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

template <int D, class T>
template <class Obj>
constexpr auto tg::halfspace<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb_boundary<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box<D, DAmbient, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box_boundary<D, DAmbient, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

// intersects(b, eps), one definition per object type

template <int D, class T>
template <class Obj>
constexpr bool tg::pos<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::segment<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::ray<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::line<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::triangle<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::plane<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::aabb<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::sphere<D, D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <class T>
template <class Obj>
constexpr bool tg::sphere<2, 3, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::sphere_boundary<D, D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <class T>
template <class Obj>
constexpr bool tg::sphere_boundary<2, 3, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr bool tg::ellipsoid<D, DAmbient, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr bool tg::ellipsoid_boundary<D, DAmbient, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::halfspace<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::aabb_boundary<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr bool tg::box<D, DAmbient, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr bool tg::box_boundary<D, DAmbient, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule_boundary<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_boundary<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_mantle<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::capsule<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::capsule_boundary<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::cylinder<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::cylinder_boundary<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::cylinder_mantle<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone_boundary<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone_mantle<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere_boundary<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere_mantle<D, T>::intersects(Obj const& obj) const
{
    return tg::impl::intersects(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::cone<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::cone_boundary<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::cone_mantle<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::hemisphere<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::hemisphere_boundary<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}

template <int D, class T>
template <class Obj>
constexpr bool tg::hemisphere_mantle<D, T>::intersects(Obj const& obj, T eps) const
{
    return tg::impl::intersects_within(*this, obj, eps);
}
