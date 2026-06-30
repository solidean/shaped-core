#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>

namespace tg
{
/// Axis-aligned bounding box in D dimensions.
///
/// Represents the solid box — the set of points {x : min[i] <= x[i] <= max[i] for all i}. It is a
/// full-dimensional, finite object (intrinsic_dim == ambient_dim == D). A well-formed aabb has
/// min <= max component-wise; this is not enforced at construction.
///
/// Default construction yields a degenerate box at the origin (min == max == origin).
///
///     tg::aabb3f b(tg::pos3f(0, 0, 0), tg::pos3f(1, 1, 1));   // the unit cube
template <int D, class T>
struct aabb
{
    static_assert(D > 0, "aabb requires a positive dimension");

    pos<D, T> min;
    pos<D, T> max;

    // construction
public:
    aabb() = default;

    explicit constexpr aabb(pos<D, T> const& min, pos<D, T> const& max) : min(min), max(max) {}

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(aabb const&, aabb const&) = default;
};

template <int D, class T>
struct object_traits<aabb<D, T>>
{
    static constexpr int intrinsic_dim = D;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

} // namespace tg
