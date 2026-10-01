#pragma once

#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/impl/bounds_of.hh>
#include <typed-geometry/geometry/impl/sampling.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/line.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Line segment between two D-dimensional endpoints.
///
/// Represents the set of points {(1 - t)*pos0 + t*pos1 : t in [0, 1]} — the straight connection between pos0 and pos1, endpoints included.
/// It is a 1D object (intrinsic_dim == 1) in D-dimensional space and is finite.
/// A segment with pos0 == pos1 is a degenerate point, which is not enforced against.
///
///     tg::segment3f s(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0));
template <int D, class T>
struct tg::segment
{
    static_assert(D > 0, "segment requires a positive dimension");

    pos<D, T> pos0;
    pos<D, T> pos1;

    // construction
public:
    segment() = default;

    explicit constexpr segment(pos<D, T> const& pos0, pos<D, T> const& pos1) : pos0(pos0), pos1(pos1) {}

    // transformation
public:
    /// A projective map keeps a segment a segment only while both endpoints stay in front of the projection.
    /// That is NOT checked: an endpoint behind it maps to its mirror image, and the result is a segment through
    /// the wrong points rather than a diagnosed error.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::affine_transform<D, T>(t); })
        {
            auto const a = tg::affine_transform<D, T>(t);
            return segment(pos0.transformed(a), pos1.transformed(a));
        }
        else if constexpr (requires { tg::projective_transform<D, T>(t); })
        {
            auto const p = tg::projective_transform<D, T>(t);
            return segment(pos0.transformed(p), pos1.transformed(p));
        }
        else
            static_assert(false, "tg: a segment can be transformed by an affine or a projective map");
    }

    // measures and readings
public:
    [[nodiscard]] constexpr T length() const
        requires(tg::traits::has_sqrt<T>)
    {
        return (pos1 - pos0).length();
    }
    [[nodiscard]] constexpr pos<D, T> centroid() const { return pos0 + (pos1 - pos0) / T(2); }
    [[nodiscard]] constexpr aabb<D, T> bounds() const { return tg::impl::bounds_of(pos0, pos1); }
    [[nodiscard]] constexpr cc::fixed_array<pos<D, T>, 2> vertices() const { return {{pos0, pos1}}; }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return pos0; }
    /// the line through both endpoints, with pos0 at parameter 0 and pos1 at 1.
    [[nodiscard]] constexpr line<D, T> unbounded() const { return line<D, T>(pos0, pos1 - pos0); }

    // parameters
public:
    /// (1 - t) * pos0 + t * pos1; the segment is t in [0, 1].
    [[nodiscard]] constexpr pos<D, T> at(T t) const { return pos0 + (pos1 - pos0) * t; }
    /// the parameter of p's projection onto the segment, so always in [0, 1].
    [[nodiscard]] constexpr T parameter_of(pos<D, T> const& p) const
    {
        auto const d = pos1 - pos0;
        auto const t = tg::dot(p - pos0, d) / tg::dot(d, d);
        return t < T(0) ? T(0) : (t > T(1) ? T(1) : t);
    }

    // sampling
public:
    [[nodiscard]] pos<D, T> sample_uniform(cc::random& rng) const
        requires(tg::impl::samplable<T>)
    {
        return this->at(rng.uniform(T(0), T(1)));
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
    template <class Obj>
    [[nodiscard]] constexpr auto intersection_parameter_with(Obj const& obj) const;
    template <class Obj>
    [[nodiscard]] constexpr auto closest_intersection_parameter_with(Obj const& obj) const;

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(segment const&, segment const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::segment<D, T>>
{
    static constexpr int intrinsic_dim = 1;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
