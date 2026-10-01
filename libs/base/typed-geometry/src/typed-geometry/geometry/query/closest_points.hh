#pragma once

#include <clean-core/container/pair.hh>
#include <typed-geometry/geometry/query/impl/gjk.hh>
#include <typed-geometry/geometry/query/impl/kernels.hh>
#include <typed-geometry/geometry/query/project.hh>

/// `a.closest_points_to(b)`: {the point of a nearest b, the point of b nearest a}.
/// `a.closest_point_to(b)`: its first half, and for a pos `b` exactly `b.project_to(a)`.

namespace tg::impl
{
template <class A, class B>
[[nodiscard]] constexpr auto closest_points_to(A const& a, B const& b)
{
    static_assert(written_once<closest_points_op, A, B>, "tg: a closest_points kernel is written in both orders");

    if constexpr (has_op<closest_points_op, A, B>)
        return closest_points_op<A, B>::apply(a, b);
    else if constexpr (has_op<closest_points_op, B, A>)
    {
        auto const r = closest_points_op<B, A>::apply(b, a);
        return cc::pair{r.second, r.first};
    }
    else if constexpr (is_pos<A> && has_project_to<A, B>)
        return cc::pair{a, impl::project_to(a, b)};
    else if constexpr (is_pos<B> && has_project_to<B, A>)
        return cc::pair{impl::project_to(b, a), b};
    else if constexpr (gjk_pair<A, B>)
    {
        auto const r = impl::gjk(a, b);
        return cc::pair{r.on_a, r.on_b};
    }
    else
        static_assert(false, "tg: no closest_points_to for this pair of types; tg::has_closest_points_to<A, B> is the "
                             "probe");
}

template <class A, class B>
[[nodiscard]] constexpr auto closest_point_to(A const& a, B const& b)
{
    if constexpr (is_pos<B> && has_project_to<B, A>)
        return impl::project_to(b, a);
    else
        return impl::closest_points_to(a, b).first;
}
} // namespace tg::impl

// closest_points_to, one definition per object type

template <int D, class T>
template <class Obj>
constexpr auto tg::pos<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::segment<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::ray<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::line<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::triangle<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::plane<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere<D, D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere<2, 3, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere_boundary<D, D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere_boundary<2, 3, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid<D, DAmbient, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid_boundary<D, DAmbient, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::halfspace<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb_boundary<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box<D, DAmbient, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box_boundary<D, DAmbient, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

// closest_point_to, one definition per object type

template <int D, class T>
template <class Obj>
constexpr auto tg::pos<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::segment<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::ray<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::line<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::triangle<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::plane<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere<D, D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere<2, 3, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::sphere_boundary<D, D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <class T>
template <class Obj>
constexpr auto tg::sphere_boundary<2, 3, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid<D, DAmbient, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::ellipsoid_boundary<D, DAmbient, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::halfspace<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::aabb_boundary<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box<D, DAmbient, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, int DAmbient, class T>
template <class Obj>
constexpr auto tg::box_boundary<D, DAmbient, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule_boundary<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_boundary<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_mantle<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::capsule_boundary<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_boundary<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cylinder_mantle<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone_boundary<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone_mantle<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere_boundary<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere_mantle<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone_boundary<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::cone_mantle<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere_boundary<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::hemisphere_mantle<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::tetrahedron<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::tetrahedron_boundary<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::quad<D, T>::closest_points_to(Obj const& obj) const
{
    return tg::impl::closest_points_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::tetrahedron<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::tetrahedron_boundary<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}

template <int D, class T>
template <class Obj>
constexpr auto tg::quad<D, T>::closest_point_to(Obj const& obj) const
{
    return tg::impl::closest_point_to(*this, obj);
}
