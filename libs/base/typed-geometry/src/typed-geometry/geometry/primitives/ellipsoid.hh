#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace tg
{
/// Ellipsoid surface, stored as a center and its D semi-axis vectors.
///
/// Represents {center + sum_i u_i * semi_axes[i] : |u| == 1} — the surface itself, not the solid interior, so intrinsic_dim is D - 1.
/// D is the dimension of the flat the ellipsoid curves in, DAmbient the dimension of the space that flat sits in, and D <= DAmbient.
/// The two differ exactly when the object is embedded above its own dimension:
/// `ellipsoid<2, 2, T>` is an ellipse in the plane, `ellipsoid<2, 3, T>` that same ellipse lying in 3D, `ellipsoid<3, 3, T>` the ordinary 3D ellipsoid.
///
/// The semi-axes need not be orthogonal, so this also covers a sheared image of a sphere.
/// They also span the flat, which is why the embedded case needs no separate orientation — unlike tg::sphere, which carries a normal there.
/// Linearly dependent semi-axes are a degenerate ellipsoid, not an error.
///
/// This is what a tg::sphere becomes under a transform that does not preserve angles.
///
///     auto const e = tg::ellipsoid3f(tg::pos3f(0, 0, 0), tg::vec3f(2, 0, 0), tg::vec3f(0, 2, 0), tg::vec3f(0, 0, 2));   // radius-2 sphere
///     auto const c = tg::ellipsoid2in3f(tg::pos3f(0, 0, 0), tg::vec3f(2, 0, 0), tg::vec3f(0, 1, 0));                    // an ellipse in the xy-plane of 3D
template <int D, int DAmbient, class T>
struct ellipsoid
{
    static_assert(D > 0, "ellipsoid requires a positive dimension");
    static_assert(D <= DAmbient, "an ellipsoid cannot curve in more dimensions than the space it is embedded in");

    pos<DAmbient, T> center;
    vec<DAmbient, T> semi_axes[D] = {};

    // construction
public:
    ellipsoid() = default;

    explicit constexpr ellipsoid(pos<DAmbient, T> const& center, vec<DAmbient, T> const& axis0)
        requires(D == 1)
      : center(center), semi_axes{axis0}
    {
    }

    explicit constexpr ellipsoid(pos<DAmbient, T> const& center,
                                 vec<DAmbient, T> const& axis0,
                                 vec<DAmbient, T> const& axis1)
        requires(D == 2)
      : center(center), semi_axes{axis0, axis1}
    {
    }

    explicit constexpr ellipsoid(pos<DAmbient, T> const& center,
                                 vec<DAmbient, T> const& axis0,
                                 vec<DAmbient, T> const& axis1,
                                 vec<DAmbient, T> const& axis2)
        requires(D == 3)
      : center(center), semi_axes{axis0, axis1, axis2}
    {
    }

    /// the semi-axes in order — the array parameter is what pins their count to D for any dimension.
    explicit constexpr ellipsoid(pos<DAmbient, T> const& center, vec<DAmbient, T> const (&axes)[D]) : center(center)
    {
        for (int i = 0; i < D; ++i)
            semi_axes[i] = axes[i];
    }

    // transformation
public:
    /// An affine map sends an ellipsoid to an ellipsoid: each semi-axis is carried along as a displacement.
    /// The map is one of the ambient space, so the embedded case is no different — the images of the semi-axes span the image flat.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::affine_transform<DAmbient, T>(t); })
        {
            auto const a = tg::affine_transform<DAmbient, T>(t);

            vec<DAmbient, T> axes[D] = {};
            for (int i = 0; i < D; ++i)
                axes[i] = semi_axes[i].transformed(a);

            return ellipsoid(center.transformed(a), axes);
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

template <int D, int DAmbient, class T>
struct object_traits<ellipsoid<D, DAmbient, T>>
{
    static constexpr int intrinsic_dim = D - 1;
    static constexpr int ambient_dim = DAmbient;
    static constexpr bool is_finite = true;
};

} // namespace tg
