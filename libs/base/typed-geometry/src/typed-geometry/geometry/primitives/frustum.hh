#pragma once

#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/impl/bounds_of.hh>
#include <typed-geometry/geometry/impl/sampling.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/plane.hh>
#include <typed-geometry/geometry/primitives/tetrahedron.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Frustum: the solid between six planes — left, right, bottom, top, near, far — each normal pointing outward.
///
/// Represents {x : dot(planes[i].normal, x) <= planes[i].dist for every i}, a convex hexahedron.
/// Only the planes are stored; `vertices()` derives the eight corners where three planes meet.
/// Each choice of one plane from left/right, bottom/top and near/far must meet in a point, which any view frustum does.
/// tg::frustum_boundary is its six faces.
template <int D, class T>
struct tg::frustum
{
    static_assert(D == 3, "a frustum is 3D");

    plane<D, T> planes[6] = {};

    // construction
public:
    frustum() = default;

    explicit constexpr frustum(plane<D, T> const& left,
                               plane<D, T> const& right,
                               plane<D, T> const& bottom,
                               plane<D, T> const& top,
                               plane<D, T> const& near_,
                               plane<D, T> const& far_)
      : planes{left, right, bottom, top, near_, far_}
    {
    }

    // readings
public:
    [[nodiscard]] constexpr frustum_boundary<D, T> boundary() const
    {
        return frustum_boundary<D, T>(planes[0], planes[1], planes[2], planes[3], planes[4], planes[5]);
    }

    /// corner i lies on the right plane if bit 0 of i is set (else the left), the top if bit 1 (else the bottom),
    /// the far if bit 2 (else the near).
    [[nodiscard]] constexpr cc::fixed_array<pos<D, T>, 8> vertices() const
    {
        cc::fixed_array<pos<D, T>, 8> r = {};
        for (int c = 0; c < 8; ++c)
        {
            auto const& a = planes[(c & 1) ? 1 : 0];
            auto const& b = planes[(c & 2) ? 3 : 2];
            auto const& f = planes[(c & 4) ? 5 : 4];
            // the meeting point of three planes, by Cramer's rule over their normals
            auto const bc = tg::dual(tg::cross(b.normal, f.normal));
            auto const ca = tg::dual(tg::cross(f.normal, a.normal));
            auto const ab = tg::dual(tg::cross(a.normal, b.normal));
            r[c] = pos<D, T>() + (bc * a.dist + ca * b.dist + ab * f.dist) / tg::dot(a.normal, bc);
        }
        return r;
    }

    // transformation
public:
    /// Every plane maps as a plane, and an affine map keeps the outward sides outward.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else
        {
            frustum r;
            for (int i = 0; i < 6; ++i)
                r.planes[i] = planes[i].transformed(t);
            return r;
        }
    }

    // measures and readings
public:
    [[nodiscard]] constexpr aabb<D, T> bounds() const
    {
        auto const v = this->vertices();
        return tg::impl::bounds_of(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    }
    /// the hexahedron split into six tetrahedra around its diagonal from corner 0 to corner 7.
    [[nodiscard]] constexpr cc::fixed_array<tetrahedron<D, T>, 6> tetrahedra() const
    {
        auto const v = this->vertices();
        return {{tetrahedron<D, T>(v[0], v[1], v[3], v[7]), tetrahedron<D, T>(v[0], v[3], v[2], v[7]),
                 tetrahedron<D, T>(v[0], v[2], v[6], v[7]), tetrahedron<D, T>(v[0], v[6], v[4], v[7]),
                 tetrahedron<D, T>(v[0], v[4], v[5], v[7]), tetrahedron<D, T>(v[0], v[5], v[1], v[7])}};
    }
    [[nodiscard]] constexpr T volume() const
    {
        auto r = T(0);
        for (auto const& t : this->tetrahedra())
            r = r + t.volume();
        return r;
    }
    [[nodiscard]] constexpr pos<D, T> centroid() const
    {
        auto weighted = vec<D, T>();
        auto total = T(0);
        for (auto const& t : this->tetrahedra())
        {
            auto const v = t.volume();
            weighted = weighted + (t.centroid() - pos<D, T>()) * v;
            total = total + v;
        }
        return pos<D, T>() + weighted / total;
    }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return this->vertices()[0]; }

    // sampling
public:
    /// by rejection from the bounds.
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        auto const b = this->bounds();
        while (true)
        {
            auto const p = b.sample_uniform(rng);
            auto inside = true;
            for (auto const& pl : planes)
                inside = inside && tg::dot(pl.normal, p - pos<D, T>()) <= pl.dist;
            if (inside)
                return p;
        }
    }

    // queries: defined per verb in geometry/query/, see libs/base/typed-geometry/docs/plans/geometry-query-matrix.md
public:
    template <class Obj>
    [[nodiscard]] constexpr auto project_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_points_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_point_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_sqr_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto signed_distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto contains(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersects(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr bool intersects(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr bool contains(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(frustum const&, frustum const&) = default;
};

/// The six faces of a tg::frustum, intrinsic_dim 2.
template <int D, class T>
struct tg::frustum_boundary
{
    static_assert(D == 3, "a frustum is 3D");

    plane<D, T> planes[6] = {};

    // construction
public:
    frustum_boundary() = default;

    explicit constexpr frustum_boundary(plane<D, T> const& left,
                                        plane<D, T> const& right,
                                        plane<D, T> const& bottom,
                                        plane<D, T> const& top,
                                        plane<D, T> const& near_,
                                        plane<D, T> const& far_)
      : planes{left, right, bottom, top, near_, far_}
    {
    }

    // readings
public:
    [[nodiscard]] constexpr frustum<D, T> solid() const
    {
        return frustum<D, T>(planes[0], planes[1], planes[2], planes[3], planes[4], planes[5]);
    }

    // transformation
public:
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else
            return this->solid().transformed(t).boundary();
    }

    // measures and readings
public:
    [[nodiscard]] constexpr aabb<D, T> bounds() const { return this->solid().bounds(); }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return this->solid().vertices()[0]; }

    // queries: defined per verb in geometry/query/, see libs/base/typed-geometry/docs/plans/geometry-query-matrix.md
public:
    template <class Obj>
    [[nodiscard]] constexpr auto project_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_points_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_point_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_sqr_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto signed_distance_to(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto contains(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersects(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr bool intersects(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr bool contains(Obj const& obj, T eps) const;
    template <class Obj>
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(frustum_boundary const&, frustum_boundary const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::frustum<D, T>>
{
    static constexpr int intrinsic_dim = 3;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

template <int D, class T>
struct tg::object_traits<tg::frustum_boundary<D, T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
