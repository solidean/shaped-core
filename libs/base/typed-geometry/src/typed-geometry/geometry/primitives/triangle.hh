#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>

namespace tg
{
/// Triangle (filled) with D-dimensional vertices.
///
/// Represents the solid triangle — the convex hull of its three vertices, i.e. the set of points
/// {a*pos0 + b*pos1 + c*pos2 : a,b,c >= 0, a+b+c == 1}. It is a 2D surface patch (intrinsic_dim ==
/// 2) living in D-dimensional space (ambient_dim == D), and it is finite. A triangle whose three
/// vertices are collinear is degenerate; this is not enforced at construction.
///
///     tg::triangle3f t(tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0));
template <int D, class T>
struct triangle
{
    static_assert(D > 0, "triangle requires a positive dimension");

    pos<D, T> pos0;
    pos<D, T> pos1;
    pos<D, T> pos2;

    // construction
public:
    triangle() = default;

    explicit constexpr triangle(pos<D, T> const& pos0, pos<D, T> const& pos1, pos<D, T> const& pos2)
      : pos0(pos0), pos1(pos1), pos2(pos2)
    {
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(triangle const&, triangle const&) = default;
};

template <int D, class T>
struct object_traits<triangle<D, T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

} // namespace tg
