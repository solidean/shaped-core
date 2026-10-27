#pragma once

#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/impl/bounds_of.hh>
#include <typed-geometry/geometry/impl/hexahedron.hh>
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
///
/// The far plane is last by convention, and it may be absent: a zero normal with dist 0, which every point satisfies.
/// That is what a reverse-Z projection with an infinite far plane gives, and `has_far_plane()` says which case this is.
/// Queries that only test planes work either way: contains, may_intersect against an object with a support, ray intervals through the solid (which stay open).
/// Those that need the corners need a far plane: vertices, bounds, volume, centroid and sampling.
/// So does everything support-based — GJK, EPA, halfspace or plane vs frustum, may_intersect against another frustum — which without one answers for the near rectangle.
/// tg::frustum_boundary is its six faces.
template <int D, class T>
struct tg::frustum
{
    using scalar_t = T;

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

    /// The frustum a view-projection matrix sees, for a reverse-Z depth range: near maps to 1, far to 0.
    ///
    /// A point is inside when its clip coordinates satisfy -w <= x <= w, -w <= y <= w and 0 <= z <= w, and each of
    /// those six inequalities is a plane in world space read off the rows of the matrix.
    /// A projection with an infinite far plane has nothing to read there, and gives the frustum an absent far plane.
    /// tg::view_projection::frustum picks the factory matching its depth convention.
    [[nodiscard]] static constexpr frustum make_from_view_projection(projective_transform<D, T> const& vp)
        requires(tg::traits::has_sqrt<T>)
    {
        // near is z <= w, far is z >= 0
        return frustum::make_from_clip_rows(vp, vec<4, T>(T(0), T(0), T(-1), T(1)), vec<4, T>(T(0), T(0), T(1), T(0)));
    }

    /// The same for a forward [0, 1] depth range, near to 0 and far to 1: D3D's, Vulkan's and Metal's classic one.
    [[nodiscard]] static constexpr frustum make_from_view_projection_zero_to_one(projective_transform<D, T> const& vp)
        requires(tg::traits::has_sqrt<T>)
    {
        return frustum::make_from_clip_rows(vp, vec<4, T>(T(0), T(0), T(1), T(0)), vec<4, T>(T(0), T(0), T(-1), T(1)));
    }

    /// The same for OpenGL's [-1, 1] depth range, near to -1 and far to 1.
    [[nodiscard]] static constexpr frustum make_from_view_projection_minus_one_to_one(projective_transform<D, T> const& vp)
        requires(tg::traits::has_sqrt<T>)
    {
        return frustum::make_from_clip_rows(vp, vec<4, T>(T(0), T(0), T(1), T(1)), vec<4, T>(T(0), T(0), T(-1), T(1)));
    }

    // readings
public:
    /// false when the far plane is absent: an infinite view frustum, unbounded behind its near plane.
    [[nodiscard]] constexpr bool has_far_plane() const
    {
        auto const& n = planes[5].normal;
        return !(tg::traits::is_zero(n.data[0]) && tg::traits::is_zero(n.data[1]) && tg::traits::is_zero(n.data[2]));
    }

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
    /// every pair of corners that share two of their three planes; needs a far plane, as vertices() does.
    [[nodiscard]] constexpr cc::fixed_array<segment<D, T>, 12> edges() const
    {
        return tg::impl::corner_edges<3, D, T>(this->vertices());
    }
    /// the six faces, wound counterclockwise seen from outside; face i lies on planes[i].
    /// Needs a far plane, as vertices() does.
    [[nodiscard]] constexpr cc::fixed_array<quad<D, T>, 6> faces() const
    {
        // The corner bits run right, top, far, which is right-handed or not depending on the planes, so the right face asks its own normal.
        auto const f = tg::impl::corner_faces(this->vertices(), false);
        auto const n = tg::dual(tg::cross(f[1].pos10 - f[1].pos00, f[1].pos01 - f[1].pos00));
        return tg::dot(n, planes[1].normal) < T(0) ? tg::impl::corner_faces(this->vertices(), true) : f;
    }

    // transformation
public:
    /// Every plane maps as a plane, and the outward sides stay outward, under a mirror too.
    /// An absent far plane stays absent: its zero normal maps to zero.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else
        {
            auto const flip = tg::impl::flips_plane_normals<D, T>(t);
            frustum r;
            for (int i = 0; i < 6; ++i)
            {
                auto const p = planes[i].transformed(t);
                r.planes[i] = flip ? plane<D, T>(-p.normal, -p.dist) : p;
            }
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
    [[nodiscard]] constexpr bool may_intersect(Obj const& obj) const;
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

private:
    /// `near_a` and `far_a` are the clip-space half-spaces a . (x, y, z, w) >= 0 the depth convention bounds depth by.
    /// A far half-space with no xyz part holds everywhere in front of the camera: an infinite far plane, left absent.
    [[nodiscard]] static constexpr frustum make_from_clip_rows(projective_transform<D, T> const& vp,
                                                               vec<4, T> const& near_a,
                                                               vec<4, T> const& far_a)
        requires(tg::traits::has_sqrt<T>)
    {
        auto const m = vp.to_mat();
        auto const row = [&](int r)
        { return vec<4, T>(m.cols[0].data[r], m.cols[1].data[r], m.cols[2].data[r], m.cols[3].data[r]); };
        auto const r0 = row(0);
        auto const r1 = row(1);
        auto const r2 = row(2);
        auto const r3 = row(3);
        // the world-space half-space of a clip-space one: a . (M (x, 1)) = (a^T M) . (x, 1)
        auto const world
            = [&](vec<4, T> const& a) { return r0 * a.data[0] + r1 * a.data[1] + r2 * a.data[2] + r3 * a.data[3]; };
        // a half-space a . (x, 1) >= 0, as a world plane with its normal pointing out
        auto const outward = [](vec<4, T> const& a)
        {
            auto const n = vec<3, T>(a.data[0], a.data[1], a.data[2]);
            auto const l = n.length();
            return plane<D, T>(-n / l, a.data[3] / l);
        };
        auto const far_world = world(far_a);
        auto const far_ = tg::traits::is_zero(far_world.data[0]) && tg::traits::is_zero(far_world.data[1])
                               && tg::traits::is_zero(far_world.data[2])
                            ? plane<D, T>(vec<D, T>(), T(0))
                            : outward(far_world);
        return frustum(outward(r3 + r0), outward(r3 - r0), outward(r3 + r1), outward(r3 - r1), outward(world(near_a)),
                       far_);
    }
};

/// The six faces of a tg::frustum, intrinsic_dim 2.
template <int D, class T>
struct tg::frustum_boundary
{
    using scalar_t = T;

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
    [[nodiscard]] constexpr bool may_intersect(Obj const& obj) const;
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
