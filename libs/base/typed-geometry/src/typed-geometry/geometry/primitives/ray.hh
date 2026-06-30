#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

namespace tg
{
/// Ray: a half-line from an origin along a direction.
///
/// Represents the set of points {origin + t*dir : t >= 0} — the origin and everything in front of
/// it along dir. It is a 1D object (intrinsic_dim == 1) in D-dimensional space and is unbounded
/// (is_finite == false). dir is expected to be non-zero and is conventionally unit-length so the
/// parameter t reads as a distance; neither is enforced at construction.
///
///     tg::ray3f r(tg::pos3f(0, 0, 0), tg::vec3f(0, 0, 1));   // ray up the +z axis
template <int D, class T>
struct ray
{
    static_assert(D > 0, "ray requires a positive dimension");

    pos<D, T> origin;
    vec<D, T> dir;

    // construction
public:
    ray() = default;

    explicit constexpr ray(pos<D, T> const& origin, vec<D, T> const& dir) : origin(origin), dir(dir) {}

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(ray const&, ray const&) = default;
};

template <int D, class T>
struct object_traits<ray<D, T>>
{
    static constexpr int intrinsic_dim = 1;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = false;
};

} // namespace tg
