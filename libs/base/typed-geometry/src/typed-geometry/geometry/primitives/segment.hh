#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace tg
{
/// Line segment between two D-dimensional endpoints.
///
/// Represents the set of points {(1 - t)*pos0 + t*pos1 : t in [0, 1]} — the straight connection between pos0 and pos1, endpoints included.
/// It is a 1D object (intrinsic_dim == 1) in D-dimensional space and is finite.
/// A segment with pos0 == pos1 is a degenerate point; this is not enforced.
///
///     tg::segment3f s(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0));
template <int D, class T>
struct segment
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

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(segment const&, segment const&) = default;
};

template <int D, class T>
struct object_traits<segment<D, T>>
{
    static constexpr int intrinsic_dim = 1;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

} // namespace tg
