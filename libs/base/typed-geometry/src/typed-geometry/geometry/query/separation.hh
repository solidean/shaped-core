#pragma once

#include <clean-core/error/optional.hh>
#include <typed-geometry/geometry/query/impl/kernels.hh>

/// `a.separation_from(b)`: how far, and which way, b has to move to stop overlapping a — empty when they do not overlap.
/// A kernel answers it where a closed form exists, EPA for any two bounded convex solids otherwise.

namespace tg::impl
{
template <class A, class B>
[[nodiscard]] constexpr auto separation_from(A const& a, B const& b)
{
    if constexpr (has_op<separation_op, A, B>)
        return separation_op<A, B>::apply(a, b);
    else if constexpr (epa_pair<A, B>)
        return impl::epa(a, b, impl::gjk(a, b));
    else
        static_assert(false, "tg: no separation_from for this pair of types; tg::has_separation_from<A, B> is the "
                             "probe");
}
} // namespace tg::impl

// separation_from, one definition per object type

template <int D, class T>
template <class Obj>
constexpr auto tg::pos<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::segment<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::ray<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::line<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::triangle<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::plane<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere<D, D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere<2, 3, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere_boundary<D, D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere_boundary<2, 3, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid<D, DAmbient, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid_boundary<D, DAmbient, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::halfspace<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb_boundary<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box<D, DAmbient, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box_boundary<D, DAmbient, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule_boundary<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_boundary<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_mantle<D, T>::separation_from(Obj const& obj) const
{
    return tg::impl::separation_from(*this, obj);
}
