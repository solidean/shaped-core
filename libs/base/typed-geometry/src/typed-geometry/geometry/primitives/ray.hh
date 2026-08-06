#pragma once

#include <typed-geometry/geometry/fwd.hh>
#include <typed-geometry/geometry/traits.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/transform/homogeneous_transform.hh>

namespace tg
{
/// Ray: a half-line from an origin along a direction.
///
/// Represents the set of points {origin + t*dir : t >= 0} — the origin and everything in front of it along dir.
/// It is a 1D object (intrinsic_dim == 1) in D-dimensional space and is unbounded (is_finite == false).
/// dir is expected to be non-zero and is conventionally unit-length, so the parameter t reads as a distance.
/// Neither is enforced at construction.
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

    // transformation
public:
    /// An affine map sends a ray to a ray.
    /// dir is transformed as a displacement and is NOT renormalized, so a non-uniform scaling rescales the ray's parameter.
    ///
    /// A projective map is deliberately not handled:
    /// a ray's point at infinity maps to a finite point, so the projective image of a ray is a bounded segment, not a ray.
    template <class TransformT>
    [[nodiscard]] constexpr auto transformed(TransformT const& t) const
    {
        if constexpr (requires { t.custom_transform(*this); })
            return t.custom_transform(*this);

        else if constexpr (requires { tg::affine_transform<D, T>(t); })
        {
            auto const a = tg::affine_transform<D, T>(t);
            return ray(origin.transformed(a), dir.transformed(a));
        }
        else
            static_assert(false, "tg: a ray only survives an affine map. Its projective image is a bounded segment, "
                                 "so transform the endpoints of the piece you care about instead.");
    }

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
