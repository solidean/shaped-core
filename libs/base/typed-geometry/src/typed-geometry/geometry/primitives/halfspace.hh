#pragma once

#include <typed-geometry/fwd.hh>
#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/primitives/plane.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/vec.hh>

/// Half-space: everything on one side of a hyperplane, the plane included.
///
/// Represents {x : dot(normal, x) <= dist} — the side the normal points away from.
/// It shares tg::plane's {normal, dist} encoding, and its boundary is exactly that plane: `h.boundary()`.
/// The normal is expected to be unit length, as a plane's is.
///
///     auto const below = tg::halfspace3f(tg::vec3f(0, 0, 1), 2.0f);   // everything with z <= 2
template <int D, class T>
struct tg::halfspace
{
    static_assert(D > 0, "halfspace requires a positive dimension");

    vec<D, T> normal;
    T dist = {};

    // construction
public:
    halfspace() = default;

    explicit constexpr halfspace(vec<D, T> const& normal, T dist) : normal(normal), dist(dist) {}

    // readings
public:
    [[nodiscard]] constexpr plane<D, T> boundary() const { return plane<D, T>(normal, dist); }

    // transformation
public:
    /// The boundary plane maps as a plane does, and the inside stays on the side the normal points away from.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);
        else
        {
            auto const p = this->boundary().transformed(t);
            return halfspace(p.normal, p.dist);
        }
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
    [[nodiscard]] friend constexpr bool operator==(halfspace const&, halfspace const&) = default;
};

template <int D, class T>
struct tg::object_traits<tg::halfspace<D, T>>
{
    static constexpr int intrinsic_dim = D;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = false;
};
