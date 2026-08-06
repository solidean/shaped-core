#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/primitives/ellipsoid.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace tg
{
/// Sphere surface in D dimensions, stored as a center and a radius.
///
/// Represents {x : distance(x, center) == radius} — the surface itself, not the solid interior, so intrinsic_dim is D - 1.
/// In 2D this is a circle.
/// The radius should not be negative.
///
/// Representation vs. interpretation: a future tg::ball will reuse this exact {center, radius}
/// encoding but denote the solid {x : distance(x, center) <= radius}, the same way plane and the
/// future halfspace share theirs.
///
///     auto const s = tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f);
///     auto const e = s.transformed(some_affine);   // an ellipsoid, not a sphere
template <int D, class T>
struct sphere
{
    static_assert(D > 0, "sphere requires a positive dimension");

    pos<D, T> center;
    T radius = {};

    // construction
public:
    sphere() = default;

    explicit constexpr sphere(pos<D, T> const& center, T radius) : center(center), radius(radius) {}

    // transformation
public:
    /// A similarity preserves angles, so a sphere stays a sphere; in 3D anything wider turns it into an ellipsoid.
    /// In 2D the wider case is an ellipse, which tg has no type for, so it is a compile error.
    ///
    /// A signed uniform scale is still sphere-preserving — in 3D it makes the similarity the FULL conformal group,
    /// since a negative scale composed with a half-turn is a plane reflection.
    /// Only that class pays for taking the magnitude; with positive factors the radius is just multiplied.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::similarity_transform<D, T>(t); })
        {
            auto const s = tg::similarity_transform<D, T>(t);
            return sphere(center.transformed(s), radius * s.uniform_scale());
        }
        else if constexpr (requires { tg::signed_similarity_transform<D, T>(t); })
        {
            auto const s = tg::signed_similarity_transform<D, T>(t);
            T const scale = s.uniform_scale();
            return sphere(center.transformed(s), radius * (scale < T(0) ? -scale : scale));
        }
        else if constexpr (requires { tg::affine_transform<D, T>(t); } && D == 3)
        {
            auto const a = tg::affine_transform<D, T>(t);
            auto const m = a.linear_mat();
            return ellipsoid<T>(center.transformed(a), m.cols[0] * radius, m.cols[1] * radius, m.cols[2] * radius);
        }
        else
            static_assert(
                false, "tg: a sphere only survives a similarity (-> sphere) or, in 3D, an affine map (-> ellipsoid). "
                       "In 2D the affine image is an ellipse, and under a projective map it is a general quadric; "
                       "tg has no type for either yet.");
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(sphere const&, sphere const&) = default;
};

template <int D, class T>
struct object_traits<sphere<D, T>>
{
    static constexpr int intrinsic_dim = D - 1;
    static constexpr int ambient_dim = D;
    static constexpr bool is_finite = true;
};

} // namespace tg
