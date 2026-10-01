#pragma once

#include <clean-core/error/optional.hh>
#include <typed-geometry/geometry/query/hits.hh>
#include <typed-geometry/geometry/query/impl/kernels.hh>
#include <typed-geometry/geometry/query/parameter.hh>

/// `a.intersection_with(b)`: the overlap of a and b, where it is a representable primitive.
/// `cc::optional<X>` of the shape the overlap has when nothing is tangent, coincident or parallel.
/// A line, ray or segment against a surface gives its crossing point (or `tg::hits` of points when it can cross more
/// than once); against a bounded solid, the segment of it inside.

namespace tg::impl
{
/// a linear object's parameter result as geometry: crossing points, or the segment inside a bounded solid.
template <class L, class B>
[[nodiscard]] constexpr auto parameters_as_points(L const& l, B const& b)
{
    constexpr int D = traits::ambient_dim<L>;
    using T = traits::scalar_t<L>;
    auto const v = impl::linear_of(l);
    auto const r = intersection_parameter_op<L, B>::apply(l, b);
    if constexpr (requires { r.has_any(); })
    {
        if constexpr (decltype(r)::capacity == 1)
            return r.has_any() ? cc::optional<pos<D, T>>(v.origin + v.dir * r.first()) : cc::optional<pos<D, T>>();
        else
            return r.mapped([&](T t) { return v.origin + v.dir * t; });
    }
    else
    {
        if (!r.has_value())
            return cc::optional<segment<D, T>>();
        return cc::optional<segment<D, T>>(
            segment<D, T>(v.origin + v.dir * r.value().start, v.origin + v.dir * r.value().end));
    }
}

template <class A, class B>
[[nodiscard]] constexpr auto intersection_with(A const& a, B const& b)
{
    if constexpr (has_op<intersection_op, A, B>)
        return intersection_op<A, B>::apply(a, b);
    else if constexpr (has_op<intersection_op, B, A>)
        return intersection_op<B, A>::apply(b, a);
    else if constexpr (tg::parameters_as_geometry<A, B>)
        return impl::parameters_as_points(a, b);
    else if constexpr (tg::parameters_as_geometry<B, A>)
        return impl::parameters_as_points(b, a);
    else
        static_assert(false, "tg: no intersection_with for this pair of types; tg::has_intersection_with<A, B> is "
                             "the probe");
}
} // namespace tg::impl

// intersection_with, one definition per object type

template <int D, class T>
template <class Obj>
constexpr auto tg::pos<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::segment<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::ray<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::line<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::triangle<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::plane<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere<D, D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere<2, 3, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere_boundary<D, D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere_boundary<2, 3, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid<D, DAmbient, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid_boundary<D, DAmbient, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::halfspace<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb_boundary<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box<D, DAmbient, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box_boundary<D, DAmbient, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule_boundary<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_boundary<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_mantle<D, T>::intersection_with(Obj const& obj) const
{
    return tg::impl::intersection_with(*this, obj);
}
