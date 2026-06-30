#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

namespace tg
{
/// Line: an infinite straight line through a point along a direction.
///
/// Represents the set of points {origin + t*dir : t in R} — unbounded in both directions. It is a
/// 1D object (intrinsic_dim == 1) in D-dimensional space and is infinite (is_finite == false).
/// Unlike a ray it extends behind the origin as well. dir is expected non-zero (conventionally
/// unit-length); this is not enforced at construction.
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
