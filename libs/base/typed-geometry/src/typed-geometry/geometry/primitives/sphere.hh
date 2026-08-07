#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/primitives/ellipsoid.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace tg
{
/// Sphere surface, stored as a center and a radius.
///
/// Represents {x : distance(x, center) == radius} inside the flat it lives in — the surface itself, not the solid interior, so intrinsic_dim is D - 1.
/// D is the dimension of that flat, DAmbient the dimension of the space the flat sits in.
/// `sphere<2, 2, T>` is a circle in the plane, `sphere<3, 3, T>` the ordinary sphere, `sphere<2, 3, T>` that same circle lying in 3D.
/// The radius should not be negative.
///
/// The primary template is left **undefined**: what a sphere has to store depends on the pair, so every supported pair is its own specialization.
/// A sphere that spans its ambient space is {center, radius}; one that does not needs the flat named as well, which {center, radius} alone does not do.
/// Asking for a pair that has no specialization — a circle in 4D, say — is an incomplete type, not a silently wrong encoding.
///
/// Representation vs. interpretation: a future tg::ball will reuse this exact {center, radius}
/// encoding but denote the solid {x : distance(x, center) <= radius}, the same way plane and the
/// future halfspace share theirs.
template <int D, int DAmbient, class T>
struct sphere;

/// A sphere spanning its ambient space: an ordinary sphere in 3D, a circle in 2D.
///
///     auto const s = tg::sphere3f(tg::pos3f(0, 0, 0), 1.0f);
///     auto const e = s.transformed(some_affine);   // an ellipsoid, not a sphere
template <int D, class T>
struct sphere<D, D, T>
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
    /// A similarity preserves angles, so a sphere stays a sphere; anything wider turns it into an ellipsoid.
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
        else if constexpr (requires { tg::affine_transform<D, T>(t); })
        {
            auto const a = tg::affine_transform<D, T>(t);
            auto const m = a.linear_mat();

            // the semi-axes are the images of the radius vectors along each axis
            vec<D, T> axes[D] = {};
            for (int i = 0; i < D; ++i)
                axes[i] = m.cols[i] * radius;

            return ellipsoid<D, D, T>(center.transformed(a), axes);
        }
        else
            static_assert(false, "tg: a sphere only survives a similarity (-> sphere) or an affine map "
                                 "(-> ellipsoid). Under a projective map it becomes a general quadric, which tg has no "
                                 "type for yet.");
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(sphere const&, sphere const&) = default;
};

/// A circle lying in 3D, which is a plane's worth of circle plus the plane.
///
/// {center, radius} says nothing about which plane the circle lies in, so this case carries the plane's normal on top.
/// The normal is expected to be unit-length; its sign is a convention, both orientations name the same circle.
///
///     auto const c = tg::sphere2in3f(tg::pos3f(0, 0, 0), 1.0f, tg::vec3f(0, 0, 1));   // unit circle in the xy-plane
template <class T>
struct sphere<2, 3, T>
{
    pos<3, T> center;
    T radius = {};
    vec<3, T> normal;

    // construction
public:
    sphere() = default;

    explicit constexpr sphere(pos<3, T> const& center, T radius, vec<3, T> const& normal)
      : center(center), radius(radius), normal(normal)
    {
    }

    // transformation
public:
    /// A similarity carries the plane along with the circle, and the linear part is its uniform scale times a rotation —
    /// so dividing the image of a unit normal by that scale re-normalizes it exactly, with no sqrt.
    /// A signed scale flips the normal, which names the same plane.
    ///
    /// The affine image is an ellipse in space — an ellipsoid<2, 3, T> — but naming its semi-axes needs an orthonormal
    /// basis of the circle's plane, which linalg has no routine for yet, so that pair is a compile error for now.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::similarity_transform<3, T>(t); })
        {
            auto const s = tg::similarity_transform<3, T>(t);
            T const scale = s.uniform_scale();
            return sphere(center.transformed(s), radius * scale, normal.transformed(s) / scale);
        }
        else if constexpr (requires { tg::signed_similarity_transform<3, T>(t); })
        {
            auto const s = tg::signed_similarity_transform<3, T>(t);
            T const scale = s.uniform_scale();
            return sphere(center.transformed(s), radius * (scale < T(0) ? -scale : scale), normal.transformed(s) / scale);
        }
        else
            static_assert(false, "tg: an embedded sphere only survives a similarity. Its affine image is an ellipse in "
                                 "space, which needs an orthonormal basis of its plane — a linalg routine tg does not "
                                 "have yet.");
    }

    // comparison
public:
    [[nodiscard]] friend constexpr bool operator==(sphere const&, sphere const&) = default;
};

template <int D, int DAmbient, class T>
struct object_traits<sphere<D, DAmbient, T>>
{
    static constexpr int intrinsic_dim = D - 1;
    static constexpr int ambient_dim = DAmbient;
    static constexpr bool is_finite = true;
};

} // namespace tg
