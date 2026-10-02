#pragma once

#include <clean-core/error/optional.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/line.hh>
#include <typed-geometry/geometry/primitives/plane.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/primitives/sphere.hh>
#include <typed-geometry/geometry/primitives/triangle.hh>
#include <typed-geometry/geometry/query/impl/ops.hh>
#include <typed-geometry/geometry/query/impl/special_case.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>

/// `intersection_with` where the overlap of two objects is itself a representable primitive.
/// Each returns the shape the overlap has when nothing is tangent, coincident or parallel.
/// Parallel or concentric objects that never meet give no overlap; a coincident pair is the special case, and gives none.

/// The overlap of two boxes is a box: the larger of the mins and the smaller of the maxes.
template <int D, class T>
struct tg::impl::intersection_op<tg::aabb<D, T>, tg::aabb<D, T>>
{
    [[nodiscard]] static constexpr cc::optional<aabb<D, T>> apply(aabb<D, T> const& a, aabb<D, T> const& b)
    {
        aabb<D, T> r;
        for (int i = 0; i < D; ++i)
        {
            r.min.data[i] = a.min.data[i] > b.min.data[i] ? a.min.data[i] : b.min.data[i];
            r.max.data[i] = a.max.data[i] < b.max.data[i] ? a.max.data[i] : b.max.data[i];
            if (r.min.data[i] > r.max.data[i])
                return {};
        }
        return r;
    }
};

/// Two planes in 3D meet in a line along the cross product of their normals.
/// Parallel planes have no line in common, coincident ones included, which are the special case.
template <class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::intersection_op<tg::plane<3, T>, tg::plane<3, T>>
{
    [[nodiscard]] static constexpr cc::optional<line<3, T>> apply(plane<3, T> const& a, plane<3, T> const& b)
    {
        auto const dir = tg::dual(tg::cross(a.normal, b.normal));
        auto const d2 = tg::dot(dir, dir);
        if (tg::traits::is_zero(d2))
        {
            // opposite normals with negated distances describe the same plane too
            TG_SPECIAL_CASE(tg::traits::is_zero(tg::dot(a.normal, b.normal) > T(0) ? a.dist - b.dist : a.dist + b.dist),
                            "two coincident planes");
            return {};
        }
        // the point of the line nearest the origin: a combination of the normals meeting both plane equations
        auto const p = (tg::dual(tg::cross(dir, a.normal)) * b.dist + tg::dual(tg::cross(b.normal, dir)) * a.dist) / d2;
        return line<3, T>(pos<3, T>() + p, dir);
    }
};

/// A triangle crosses a plane in a segment between the two edges whose ends lie on opposite sides.
template <class T>
    requires(!tg::traits::is_exact<T>)
struct tg::impl::intersection_op<tg::triangle<3, T>, tg::plane<3, T>>
{
    [[nodiscard]] static constexpr cc::optional<segment<3, T>> apply(triangle<3, T> const& t, plane<3, T> const& pl)
    {
        pos<3, T> const v[3] = {t.pos0, t.pos1, t.pos2};
        T d[3] = {};
        for (int i = 0; i < 3; ++i)
            d[i] = tg::dot(pl.normal, v[i] - pos<3, T>()) - pl.dist;

        pos<3, T> ends[2] = {};
        auto n = 0;
        for (int i = 0; i < 3 && n < 2; ++i)
        {
            auto const j = (i + 1) % 3;
            if ((d[i] <= T(0)) != (d[j] <= T(0)))
                ends[n++] = v[i] + (v[j] - v[i]) * (d[i] / (d[i] - d[j]));
        }
        if (n < 2)
            return {};
        return segment<3, T>(ends[0], ends[1]);
    }
};

/// A ball cut by a plane is a disk in that plane.
template <class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::intersection_op<tg::sphere<3, 3, T>, tg::plane<3, T>>
{
    [[nodiscard]] static constexpr cc::optional<sphere<2, 3, T>> apply(sphere<3, 3, T> const& s, plane<3, T> const& pl)
    {
        auto const off = tg::dot(pl.normal, s.center - pos<3, T>()) - pl.dist;
        if (off * off > s.radius * s.radius)
            return {};
        return sphere<2, 3, T>(s.center - pl.normal * off, tg::sqrt(s.radius * s.radius - off * off), pl.normal);
    }
};

/// A sphere surface cut by a plane is a circle in that plane.
template <class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::intersection_op<tg::sphere_boundary<3, 3, T>, tg::plane<3, T>>
{
    [[nodiscard]] static constexpr cc::optional<sphere_boundary<2, 3, T>> apply(sphere_boundary<3, 3, T> const& s,
                                                                                plane<3, T> const& pl)
    {
        auto const disk = intersection_op<sphere<3, 3, T>, plane<3, T>>::apply(s.solid(), pl);
        if (!disk.has_value())
            return {};
        return disk.value().boundary();
    }
};

/// Two sphere surfaces meet in a circle, in the plane where the powers of a point with respect to both are equal.
/// Concentric spheres meet in no circle; equal ones are the special case, and give none either.
template <class T>
    requires(tg::traits::has_sqrt<T>)
struct tg::impl::intersection_op<tg::sphere_boundary<3, 3, T>, tg::sphere_boundary<3, 3, T>>
{
    [[nodiscard]] static constexpr cc::optional<sphere_boundary<2, 3, T>> apply(sphere_boundary<3, 3, T> const& a,
                                                                                sphere_boundary<3, 3, T> const& b)
    {
        auto const between = b.center - a.center;
        auto const d = between.length();
        auto const ra = a.radius;
        auto const rb = b.radius;
        if (tg::traits::is_zero(d))
        {
            TG_SPECIAL_CASE(tg::traits::is_zero(ra - rb), "two coincident spheres");
            return {};
        }
        if (d > ra + rb || d < (ra > rb ? ra - rb : rb - ra))
            return {};
        auto const n = between / d;
        auto const along = (d * d + ra * ra - rb * rb) / (T(2) * d);
        auto const h2 = ra * ra - along * along;
        return sphere_boundary<2, 3, T>(a.center + n * along, tg::sqrt(h2 > T(0) ? h2 : T(0)), n);
    }
};
