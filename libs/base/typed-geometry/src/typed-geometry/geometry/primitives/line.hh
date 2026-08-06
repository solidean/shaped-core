#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace tg
{
/// Line: an infinite straight line through a point along a direction.
///
/// Represents the set of points {origin + t*dir : t in R} — unbounded in both directions.
/// It is a 1D object (intrinsic_dim == 1) in D-dimensional space and is infinite (is_finite == false).
/// Unlike a ray it extends behind the origin as well.
/// dir is expected non-zero (conventionally unit-length); this is not enforced at construction.
///
///     tg::line3f l(tg::pos3f(0, 0, 0), tg::vec3f(1, 0, 0));   // the x axis
template <int D, class T>
struct line
{
    static_assert(D > 0, "line requires a positive dimension");

    pos<D, T> origin;
    vec<D, T> dir;

    // construction
public:
    line() = default;

    explicit constexpr line(pos<D, T> const& origin, vec<D, T> const& dir) : origin(origin), dir(dir) {}

    // transformation
public:
    /// An affine map sends a line to a line.
    /// dir is transformed as a displacement and is NOT renormalized, so a non-uniform scaling rescales the line's parameter.
    ///
    /// A projective map is deliberately not handled.
    /// A projectivity does send a projective line to a projective line,
    /// but the affine image of an affine line is a full line only when the line misses the w = 0 plane;
    /// otherwise it is a line with a point removed.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::affine_transform<D, T>(t); })
        {
            auto const a = tg::affine_transform<D, T>(t);
            return line(origin.transformed(a), dir.transformed(a));
        }
        else
            static_assert(false, "tg: a line only survives an affine map");
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(line const&, line const&) = default;
};

template <int D, class T>
struct object_traits<line<D, T>>
{
    static constexpr int intrinsic_dim = 1;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = false;
};

} // namespace tg
