#pragma once

#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/primitives/box.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Axis-aligned bounding box in D dimensions.
///
/// Represents the solid box — the set of points {x : min[i] <= x[i] <= max[i] for all i}. It is a
/// full-dimensional, finite object (intrinsic_dim == ambient_dim == D). A well-formed aabb has
/// min <= max component-wise; this is not enforced at construction.
///
/// Default construction yields a degenerate box at the origin (min == max == origin).
/// tg::aabb_boundary shares the encoding and denotes the faces only; `.boundary()` and `.solid()` convert.
///
///     tg::aabb3f b(tg::pos3f(0, 0, 0), tg::pos3f(1, 1, 1));   // the unit cube
template <int D, class T>
struct tg::aabb
{
    static_assert(D > 0, "aabb requires a positive dimension");

    pos<D, T> min;
    pos<D, T> max;

    // construction
public:
    aabb() = default;

    explicit constexpr aabb(pos<D, T> const& min, pos<D, T> const& max) : min(min), max(max) {}

    // readings
public:
    [[nodiscard]] constexpr aabb_boundary<D, T> boundary() const { return aabb_boundary<D, T>(min, max); }

    // transformation
public:
    /// An aabb stays an aabb under the axis-aligned transforms — scaling and translation.
    /// Any other affine map makes it an oriented tg::box, never a silently enlarged aabb.
    ///
    /// With positive scale factors the corners keep their order, so the image is just the two images.
    /// Only a signed scaling can swap min and max along an axis, and only that case pays for the re-sort.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::scaling_translation_transform<D, T>(t); })
        {
            auto const s = tg::scaling_translation_transform<D, T>(t);
            return aabb(min.transformed(s), max.transformed(s));
        }
        else if constexpr (requires { tg::signed_scaling_translation_transform<D, T>(t); })
        {
            auto const s = tg::signed_scaling_translation_transform<D, T>(t);
            auto const a = min.transformed(s);
            auto const b = max.transformed(s);

            aabb result;
            for (int i = 0; i < D; ++i)
            {
                bool const ordered = a.data[i] < b.data[i];
                result.min.data[i] = ordered ? a.data[i] : b.data[i];
                result.max.data[i] = ordered ? b.data[i] : a.data[i];
            }
            return result;
        }
        else if constexpr (requires { tg::affine_transform<D, T>(t); })
        {
            auto const a = tg::affine_transform<D, T>(t);
            auto const l = a.linear_mat();
            mat<D, D, T> h;
            for (int i = 0; i < D; ++i)
                h.cols[i] = l.cols[i] * ((max.data[i] - min.data[i]) / T(2));
            auto const center = min + (max - min) / T(2);
            return box<D, D, T>(center.transformed(a), h);
        }
        else
            static_assert(false, "tg: an aabb survives an affine map (as a tg::box at worst); its projective image "
                                 "has no tg type.");
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
    [[nodiscard]] friend constexpr bool operator==(aabb const&, aabb const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::aabb<D, T>>
{
    static constexpr int intrinsic_dim = D;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

/// The boundary of a tg::aabb: the points of the box on at least one face, so intrinsic_dim is D - 1.
template <int D, class T>
struct tg::aabb_boundary
{
    static_assert(D > 0, "aabb_boundary requires a positive dimension");

    pos<D, T> min;
    pos<D, T> max;

    // construction
public:
    aabb_boundary() = default;

    explicit constexpr aabb_boundary(pos<D, T> const& min, pos<D, T> const& max) : min(min), max(max) {}

    // readings
public:
    [[nodiscard]] constexpr aabb<D, T> solid() const { return aabb<D, T>(min, max); }

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
    [[nodiscard]] friend constexpr bool operator==(aabb_boundary const&, aabb_boundary const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::aabb_boundary<D, T>>
{
    static constexpr int intrinsic_dim = D - 1;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
