#pragma once

#include <clean-core/container/fixed_array.hh>
#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/impl/bounds_of.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/comp.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

/// Quad: the bilinear patch spanned by four corners, which need not lie in one plane.
///
/// Represents {at(c) : c in [0, 1]^2}, where `at` interpolates pos00 -> pos10 along the first coordinate and
/// pos00 -> pos01 along the second, so pos11 is the corner opposite pos00.
/// A non-planar patch is curved and not convex, so it has no support and no closed-form area: it gets evaluation,
/// decomposition, bounds and ray crossings, and nothing that assumes it is flat.
template <int D, class T>
struct tg::quad
{
    static_assert(D > 0, "quad requires a positive dimension");

    pos<D, T> pos00;
    pos<D, T> pos10;
    pos<D, T> pos11;
    pos<D, T> pos01;

    // construction
public:
    quad() = default;

    /// the corners in order around the patch: 00, 10, 11, 01.
    explicit constexpr quad(pos<D, T> const& p00, pos<D, T> const& p10, pos<D, T> const& p11, pos<D, T> const& p01)
      : pos00(p00), pos10(p10), pos11(p11), pos01(p01)
    {
    }

    // transformation
public:
    /// A bilinear patch maps to a bilinear patch under an affine map, corner by corner.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else if constexpr (requires { tg::affine_transform<D, T>(t); })
        {
            auto const a = tg::affine_transform<D, T>(t);
            return quad(pos00.transformed(a), pos10.transformed(a), pos11.transformed(a), pos01.transformed(a));
        }
        else
            static_assert(false, "tg: a quad is transformed by an affine map; a projective map does not keep a "
                                 "bilinear patch bilinear.");
    }

    // measures and readings
public:
    /// the patch lies in the hull of its corners.
    [[nodiscard]] constexpr aabb<D, T> bounds() const { return tg::impl::bounds_of(pos00, pos10, pos11, pos01); }
    [[nodiscard]] constexpr pos<D, T> any_point() const { return pos00; }
    [[nodiscard]] constexpr cc::fixed_array<pos<D, T>, 4> vertices() const { return {{pos00, pos10, pos11, pos01}}; }
    [[nodiscard]] constexpr cc::fixed_array<segment<D, T>, 4> edges() const
    {
        return {{segment<D, T>(pos00, pos10), segment<D, T>(pos10, pos11), segment<D, T>(pos11, pos01),
                 segment<D, T>(pos01, pos00)}};
    }

    // parameters
public:
    /// the bilinear interpolation of the corners; the patch is c in [0, 1]^2.
    [[nodiscard]] constexpr pos<D, T> at(comp<2, T> const& c) const
    {
        auto const u = c.data[0];
        auto const v = c.data[1];
        auto const bottom = pos00 + (pos10 - pos00) * u;
        auto const top = pos01 + (pos11 - pos01) * u;
        return bottom + (top - bottom) * v;
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
    [[nodiscard]] friend constexpr bool operator==(quad const&, quad const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::quad<D, T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};
