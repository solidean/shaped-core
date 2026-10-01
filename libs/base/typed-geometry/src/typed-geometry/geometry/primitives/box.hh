#pragma once

#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/comp.hh>
#include <typed-geometry/linalg/cross.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/scalar/scalar.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Oriented box, stored as a center and a half-extent matrix whose columns are the half-axes.
///
/// Represents {center + H * c : c in [-1, 1]^D}, interior included, so intrinsic_dim is D.
/// The half-axes need not be orthogonal: any affine image of a box is a box (a parallelepiped), which is why the
/// representation is the matrix rather than a rotation and three lengths.
/// D is the dimension of the flat the box spans, DAmbient the space it sits in; `box<2, 3, T>` is a rectangle in 3D.
/// tg::box_boundary shares the encoding and denotes the faces only.
///
///     auto const h = tg::mat3f::make_from_cols(tg::vec3f(2, 0, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, 1));
///     auto const b = tg::box3f(tg::pos3f(0, 0, 0), h);   // 4 x 2 x 2, centered at the origin
template <int D, int DAmbient, class T>
struct tg::box
{
    static_assert(D > 0, "box requires a positive dimension");
    static_assert(D <= DAmbient, "a box cannot span more dimensions than the space it is embedded in");

    pos<DAmbient, T> center;
    mat<D, DAmbient, T> half_extents;

    // construction
public:
    box() = default;

    explicit constexpr box(pos<DAmbient, T> const& center, mat<D, DAmbient, T> const& half_extents)
      : center(center), half_extents(half_extents)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr box_boundary<D, DAmbient, T> boundary() const
    {
        return box_boundary<D, DAmbient, T>(center, half_extents);
    }

    // transformation
public:
    /// An affine map carries the center as a point and every half-axis as a displacement.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::affine_transform<DAmbient, T>(t); })
        {
            auto const a = tg::affine_transform<DAmbient, T>(t);
            return box(center.transformed(a), a.linear_mat() * half_extents);
        }
        else
            static_assert(false, "tg: a box only survives an affine map; its projective image is a frustum-like "
                                 "hexahedron, which tg has no type for.");
    }

    // measures and readings
public:
    [[nodiscard]] constexpr pos<DAmbient, T> centroid() const { return center; }
    /// the center pushed out by every half-axis's reach along each coordinate.
    [[nodiscard]] constexpr aabb<DAmbient, T> bounds() const
    {
        auto e = vec<DAmbient, T>();
        for (int i = 0; i < D; ++i)
            for (int k = 0; k < DAmbient; ++k)
                e.data[k] = e.data[k] + tg::abs(half_extents.cols[i].data[k]);
        return aabb<DAmbient, T>(center - e, center + e);
    }
    [[nodiscard]] constexpr pos<DAmbient, T> any_point() const { return center; }
    /// in 2D the area; in 3D the area of the six faces.
    [[nodiscard]] constexpr T area() const
        requires((D == 2 || D == 3) && tg::traits::has_sqrt<T>)
    {
        auto const& h = half_extents.cols;
        if constexpr (D == 2 && DAmbient == 2)
            return T(4) * tg::abs(h[0].data[0] * h[1].data[1] - h[0].data[1] * h[1].data[0]);
        else if constexpr (D == 2)
            return T(4) * tg::dual(tg::cross(h[0], h[1])).length();
        else
            return T(8)
                 * (tg::dual(tg::cross(h[0], h[1])).length() + tg::dual(tg::cross(h[1], h[2])).length()
                    + tg::dual(tg::cross(h[2], h[0])).length());
    }
    [[nodiscard]] constexpr T perimeter() const
        requires(D == 2 && tg::traits::has_sqrt<T>)
    {
        return T(4) * (half_extents.cols[0].length() + half_extents.cols[1].length());
    }
    [[nodiscard]] constexpr T volume() const
        requires(D == 3 && DAmbient == 3)
    {
        return T(8) * tg::abs(half_extents.determinant());
    }
    /// unit normal of a rectangle in 3D.
    [[nodiscard]] constexpr vec<3, T> normal() const
        requires(D == 2 && DAmbient == 3 && tg::traits::has_sqrt<T>)
    {
        return tg::dual(tg::cross(half_extents.cols[0], half_extents.cols[1])).normalized();
    }
    /// corner i takes +half-axis j where bit j of i is set, -half-axis otherwise.
    [[nodiscard]] constexpr cc::fixed_array<pos<DAmbient, T>, (1 << D)> vertices() const
    {
        cc::fixed_array<pos<DAmbient, T>, (1 << D)> r = {};
        for (int v = 0; v < (1 << D); ++v)
        {
            auto p = center;
            for (int i = 0; i < D; ++i)
                p = (v >> i) & 1 ? p + half_extents.cols[i] : p - half_extents.cols[i];
            r[v] = p;
        }
        return r;
    }

    // parameters
public:
    /// center + H * c; the box is c in [-1, 1]^D.
    [[nodiscard]] constexpr pos<DAmbient, T> at(comp<D, T> const& c) const
    {
        auto p = center;
        for (int i = 0; i < D; ++i)
            p = p + half_extents.cols[i] * c.data[i];
        return p;
    }
    /// the inverse of at, unclamped: outside the box a coordinate leaves [-1, 1].
    [[nodiscard]] constexpr comp<D, T> parameter_of(pos<DAmbient, T> const& p) const
        requires(D == DAmbient)
    {
        auto const v = half_extents.inverse() * (p - center);
        comp<D, T> r;
        for (int i = 0; i < D; ++i)
            r.data[i] = v.data[i];
        return r;
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
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(box const&, box const&) = default;
};

/// The boundary of a tg::box: {center + H * c : c in [-1, 1]^D, some |c_i| == 1}, so intrinsic_dim is D - 1.
template <int D, int DAmbient, class T>
struct tg::box_boundary
{
    static_assert(D > 0, "box_boundary requires a positive dimension");
    static_assert(D <= DAmbient, "a box cannot span more dimensions than the space it is embedded in");

    pos<DAmbient, T> center;
    mat<D, DAmbient, T> half_extents;

    // construction
public:
    box_boundary() = default;

    explicit constexpr box_boundary(pos<DAmbient, T> const& center, mat<D, DAmbient, T> const& half_extents)
      : center(center), half_extents(half_extents)
    {
    }

    // readings
public:
    [[nodiscard]] constexpr box<D, DAmbient, T> solid() const { return box<D, DAmbient, T>(center, half_extents); }

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
    [[nodiscard]] constexpr pos<DAmbient, T> centroid() const { return center; }
    /// the center pushed out by every half-axis's reach along each coordinate.
    [[nodiscard]] constexpr aabb<DAmbient, T> bounds() const
    {
        auto e = vec<DAmbient, T>();
        for (int i = 0; i < D; ++i)
            for (int k = 0; k < DAmbient; ++k)
                e.data[k] = e.data[k] + tg::abs(half_extents.cols[i].data[k]);
        return aabb<DAmbient, T>(center - e, center + e);
    }
    [[nodiscard]] constexpr pos<DAmbient, T> any_point() const { return center + half_extents.cols[0]; }
    /// the faces of a 2D box are its outline.
    [[nodiscard]] constexpr T length() const
        requires(D == 2 && tg::traits::has_sqrt<T>)
    {
        return this->solid().perimeter();
    }
    [[nodiscard]] constexpr T area() const
        requires(D == 3 && tg::traits::has_sqrt<T>)
    {
        return this->solid().area();
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
    [[nodiscard]] constexpr auto separation_from(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(box_boundary const&, box_boundary const&) = default;
};

template <int D, int DAmbient, class T>
struct tg::object_traits<tg::box<D, DAmbient, T>>
{
    static constexpr int intrinsic_dim = D;
    static constexpr int ambient_dim = DAmbient;
    static constexpr bool is_finite = true;
};

template <int D, int DAmbient, class T>
struct tg::object_traits<tg::box_boundary<D, DAmbient, T>>
{
    static constexpr int intrinsic_dim = D - 1;
    static constexpr int ambient_dim = DAmbient;
    static constexpr bool is_finite = true;
};

// bounds() returns an aabb; included last because aabb.hh includes this header for its own transformed().
#include <typed-geometry/geometry/primitives/aabb.hh>
