#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/vec.hh>

namespace tg
{
/// Hyperplane in D dimensions, stored in Hesse normal form.
///
/// Represents the set of points {x : dot(normal, x) == dist} — the plane *itself*, not a side of
/// it. dist is the signed offset of the plane from the origin measured along normal, so the
/// closest point to the origin is dist * normal (when normal is unit-length). It is a
/// codimension-1 object (intrinsic_dim == D - 1) in D-dimensional space and is unbounded
/// (is_finite == false). In 2D this is a line; in 3D an ordinary plane.
///
/// normal is expected to be unit-length so that dist and dot(normal, x) read as true distances;
/// this is not enforced at construction.
///
/// Representation vs. interpretation: a future tg::halfspace will reuse this exact {normal, dist}
/// encoding but denote {x : dot(normal, x) <= dist} — one side of the plane rather than the plane.
///
///     tg::plane3f p(tg::vec3f(0, 0, 1), 5.0f);   // z == 5
template <int D, class T>
struct plane
{
    static_assert(D > 0, "plane requires a positive dimension");

    vec<D, T> normal;
    T dist = {};

    // construction
public:
    plane() = default;

    explicit constexpr plane(vec<D, T> const& normal, T dist) : normal(normal), dist(dist) {}

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(plane const&, plane const&) = default;
};

template <int D, class T>
struct object_traits<plane<D, T>>
{
    static constexpr int intrinsic_dim = D - 1;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = false;
};

} // namespace tg
