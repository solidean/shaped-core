#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace tg
{
/// Ellipsoid surface in 3D, stored as a center and its three semi-axis vectors.
///
/// Represents {center + u0 * semi_axes[0] + u1 * semi_axes[1] + u2 * semi_axes[2] : |u| == 1} — the surface itself, not the solid interior, so intrinsic_dim is 2.
/// The semi-axes need not be orthogonal, so this also covers a sheared image of a sphere.
///
/// 3D only: the 2D counterpart is an ellipse, which tg has no type for yet.
///
/// This is what a tg::sphere becomes under a transform that does not preserve angles.
///
///     auto const e = tg::ellipsoid3f(tg::pos3f(0, 0, 0), tg::vec3f(2, 0, 0), tg::vec3f(0, 2, 0), tg::vec3f(0, 0, 2));   // radius-2 sphere
template <class T>
struct ellipsoid
{
    pos<3, T> center;
    vec<3, T> semi_axes[3] = {};

    // construction
public:
    ellipsoid() = default;

    explicit constexpr ellipsoid(pos<3, T> const& center,
                                 vec<3, T> const& axis0,
                                 vec<3, T> const& axis1,
                                 vec<3, T> const& axis2)
      : center(center), semi_axes{axis0, axis1, axis2}
    {
    }

    // transformation
public:
    /// An affine map sends an ellipsoid to an ellipsoid: each semi-axis is carried along as a displacement.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::affine_transform<3, T>(t); })
        {
            auto const a = tg::affine_transform<3, T>(t);
            return ellipsoid(center.transformed(a), semi_axes[0].transformed(a), semi_axes[1].transformed(a),
                             semi_axes[2].transformed(a));
        }
        else
            static_assert(false,
                          "tg: an ellipsoid only survives an affine map. Under a projective map it becomes a general "
                          "quadric, which tg has no type for yet.");
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(ellipsoid const&, ellipsoid const&) = default;
};

template <class T>
struct object_traits<ellipsoid<T>>
{
    static constexpr int intrinsic_dim = 2;
    static constexpr int ambient_dim = 3;
    static constexpr bool is_finite = true;
};

} // namespace tg
