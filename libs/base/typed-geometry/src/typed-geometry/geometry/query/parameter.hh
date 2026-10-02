#pragma once

#include <clean-core/error/optional.hh>
#include <typed-geometry/geometry/query/hits.hh>
#include <typed-geometry/geometry/query/impl/kernels.hh>

/// `l.intersection_parameter_with(b)`: where a line, ray or segment meets b, as parameters along it —
/// `tg::hits` for a surface, `cc::optional<tg::hit_interval>` for a solid.
/// `l.closest_intersection_parameter_with(b)`: the first of them, or for a solid where l enters it (its start when inside).

namespace tg::impl
{
template <class A, class B>
[[nodiscard]] constexpr auto intersection_parameter_with(A const& a, B const& b)
{
    if constexpr (has_op<intersection_parameter_op, A, B>)
        return intersection_parameter_op<A, B>::apply(a, b);
    else
        static_assert(false, "tg: no intersection_parameter_with for this pair of types; "
                             "tg::has_intersection_parameter_with<A, B> is the probe");
}

template <class A, class B>
[[nodiscard]] constexpr auto closest_intersection_parameter_with(A const& a, B const& b)
{
    auto const r = impl::intersection_parameter_with(a, b);
    using T = traits::scalar_t<A>;
    if constexpr (requires { r.has_any(); })
        return r.has_any() ? cc::optional<T>(r.first()) : cc::optional<T>();
    else
        return r.has_value() ? cc::optional<T>(r.value().start) : cc::optional<T>();
}
} // namespace tg::impl

// the parameter verbs, one definition per linear object

template <int D, class T>
template <class Obj>
constexpr auto tg::segment<D, T>::intersection_parameter_with(Obj const& obj) const
{
    return tg::impl::intersection_parameter_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::ray<D, T>::intersection_parameter_with(Obj const& obj) const
{
    return tg::impl::intersection_parameter_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::line<D, T>::intersection_parameter_with(Obj const& obj) const
{
    return tg::impl::intersection_parameter_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::segment<D, T>::closest_intersection_parameter_with(Obj const& obj) const
{
    return tg::impl::closest_intersection_parameter_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::ray<D, T>::closest_intersection_parameter_with(Obj const& obj) const
{
    return tg::impl::closest_intersection_parameter_with(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::line<D, T>::closest_intersection_parameter_with(Obj const& obj) const
{
    return tg::impl::closest_intersection_parameter_with(*this, obj);
}
