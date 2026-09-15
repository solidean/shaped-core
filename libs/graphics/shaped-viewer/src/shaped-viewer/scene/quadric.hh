#pragma once

#include <clean-core/error/optional.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/geometry/primitives/ray.hh>
#include <typed-geometry/geometry/primitives/segment.hh>
#include <typed-geometry/geometry/primitives/sphere.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

/// A quadric surface, as the 10 distinct entries of the symmetric 4x4 Q over homogeneous points.
///
/// The surface is the zero set of
///
///     Q(p) = pᵀ A p + 2 bᵀ p + c
///
/// with A the symmetric 3x3 block (`diag` on the diagonal, `off_diag` off it), b = `linear` and c = `constant`.
/// Spheres, ellipsoids, cylinders, cones, paraboloids and plane pairs are all of this form, which is why one record and one
/// intersection routine cover the set.
///
/// **p is relative to the owning `quadric_primitive::origin`, never a world position.**
/// That is a correctness requirement rather than a convention: a sphere of radius 0.008 at world x = 5000 has constant term
/// 2.5e7 - 6.4e-5, and the ulp of 2.5e7 in float32 is about 2, so the radius is gone before anything reads it.
///
/// The factor 2 on the off-diagonal and linear terms is folded into the evaluation rather than into the storage, so
/// `off_diag` holds A's entries themselves — A_xy, A_xz, A_yz, in that order.
struct sv::quadric3
{
    tg::vec3f diag = {};     ///< A_xx, A_yy, A_zz
    tg::vec3f off_diag = {}; ///< A_xy, A_xz, A_yz
    tg::vec3f linear = {};   ///< b
    float constant = 0.0f;   ///< c

    /// Q(p), where `p` is the DISPLACEMENT from the primitive's origin rather than a world position.
    /// Taking a displacement is what makes the relative-to-origin rule a type fact instead of a comment.
    /// Negative is inside for every quadric this library builds, which is what makes a clipper's test a single sign check.
    [[nodiscard]] constexpr float evaluate(tg::vec3f const& p) const
    {
        return diag[0] * p[0] * p[0] + diag[1] * p[1] * p[1] + diag[2] * p[2] * p[2]
             + 2.0f * (off_diag[0] * p[0] * p[1] + off_diag[1] * p[0] * p[2] + off_diag[2] * p[1] * p[2])
             + 2.0f * (linear[0] * p[0] + linear[1] * p[1] + linear[2] * p[2]) + constant;
    }

    /// ∇Q(p) = 2 (A p + b), which is the outward surface normal wherever it is non-zero.
    /// Unnormalized: a caller that wants a unit normal normalizes, and one that only wants a sign does not pay for it.
    [[nodiscard]] constexpr tg::vec3f gradient(tg::vec3f const& p) const
    {
        return tg::vec3f(2.0f * (diag[0] * p[0] + off_diag[0] * p[1] + off_diag[1] * p[2] + linear[0]),
                         2.0f * (off_diag[0] * p[0] + diag[1] * p[1] + off_diag[2] * p[2] + linear[1]),
                         2.0f * (off_diag[1] * p[0] + off_diag[2] * p[1] + diag[2] * p[2] + linear[2]));
    }

    /// The quadric that admits every point, for a primitive with nothing to clip it.
    /// A negative constant and no other term, so `evaluate` is that constant everywhere.
    [[nodiscard]] static constexpr quadric3 everywhere() { return {.constant = -1.0f}; }

    /// The sphere of radius `radius` about the origin: |p|² - radius².
    [[nodiscard]] static constexpr quadric3 sphere_about_origin(float radius)
    {
        return {.diag = tg::vec3f(1.0f, 1.0f, 1.0f), .constant = -radius * radius};
    }

    /// The infinite cylinder of radius `radius` about the line through the origin along unit `axis`:
    /// |p|² - (p·axis)² - radius², whose A is I - axis axisᵀ.
    /// `axis` must be unit length.
    [[nodiscard]] static constexpr quadric3 cylinder_about_origin(tg::vec3f const& axis, float radius)
    {
        return {.diag = tg::vec3f(1.0f - axis[0] * axis[0], 1.0f - axis[1] * axis[1], 1.0f - axis[2] * axis[2]),
                .off_diag = tg::vec3f(-axis[0] * axis[1], -axis[0] * axis[2], -axis[1] * axis[2]),
                .constant = -radius * radius};
    }

    /// The slab of half-height `half_height` about the origin, normal to unit `axis`: (p·axis)² - half_height².
    ///
    /// This is the clipper that turns an infinite cylinder into a finite one, and its A is axis axisᵀ.
    /// It is also where `per_quadric_end` reads its blend parameter from, since a slab IS an axis, an offset and a half-length.
    /// `axis` must be unit length.
    [[nodiscard]] static constexpr quadric3 slab_about_origin(tg::vec3f const& axis, float half_height)
    {
        return {.diag = tg::vec3f(axis[0] * axis[0], axis[1] * axis[1], axis[2] * axis[2]),
                .off_diag = tg::vec3f(axis[0] * axis[1], axis[0] * axis[2], axis[1] * axis[2]),
                .constant = -half_height * half_height};
    }

    [[nodiscard]] friend constexpr bool operator==(quadric3 const&, quadric3 const&) = default;
};

/// One drawn primitive: a surface quadric, the region a hit has to lie in, and the object-space box both are expressed about.
///
/// A quadric surface is infinite where a drawn shape is not, so the pair is the representation rather than the surface alone.
/// A hit at p is kept exactly when `surface.evaluate(p) == 0` and `clip.evaluate(p) <= 0`, with p relative to `origin`.
///
/// The clipper being a full quadric rather than a plane is what closes this under the shapes that matter.
/// A slab is itself a quadric, so a finite cylinder is a cylinder clipped by a slab, a cone frustum is a cone clipped by a slab,
/// and a hemisphere is a sphere clipped by a plane pair — all in one record and one intersection routine.
///
/// `bounds` is world-space and is what the procedural BLAS is built from.
/// It is computed by the factory that knows the shape rather than derived from the coefficients, because a general quadric has no
/// finite box at all and only the construction knows which bounded shape was meant.
struct sv::quadric_primitive
{
    tg::pos3f origin = {};
    sv::quadric3 surface = {};
    sv::quadric3 clip = sv::quadric3::everywhere();

    /// world-space, and tight for every shape the factories below build
    tg::aabb3f bounds = {};

    /// The sphere `s`, unclipped.
    [[nodiscard]] static quadric_primitive create_sphere(tg::sphere3f const& s);

    /// The segment `s` thickened by `radius`, as a cylinder clipped to the segment's own slab.
    ///
    /// This is an OPEN tube: the ends are where the clipper cuts, and nothing closes them.
    /// A round-capped edge is this plus a sphere at each end, which `append_capsule` writes.
    /// A degenerate segment — both endpoints equal — yields a sphere instead, since there is no axis to build a cylinder about.
    [[nodiscard]] static quadric_primitive create_cylinder(tg::segment3f const& s, float radius);

    /// Whether `p`, given in world space, lies in the region the clipper admits.
    [[nodiscard]] bool admits(tg::pos3f const& p) const { return clip.evaluate(p - origin) <= 0.0f; }

    /// The outward unit normal at a world-space point on the surface.
    /// Undefined where the gradient vanishes, which for the shapes built here is only a degenerate primitive.
    [[nodiscard]] tg::vec3f normal_at(tg::pos3f const& p) const;
};

/// What a ray hit on a quadric primitive is: how far along, and the outward unit normal there.
struct sv::quadric_hit
{
    float t = 0.0f;
    tg::vec3f normal = {};
};

namespace sv
{
/// The CPU reference for what the intersection shader reports, and the shape the two are tested against each other by.
///
/// Reports the nearest root at or beyond `t_min` that the clipper admits, and otherwise the far root under the same two tests.
/// The far-root fallback is load-bearing for ordinary geometry rather than only for interior views: a slab-clipped cylinder is an
/// open tube, so seen near end-on its near root lies outside the slab and the visible surface is the inside of the far wall.
///
/// `ray.dir` need not be normalized; `t` is in units of `ray.dir`, as `TraceRay` reports it.
[[nodiscard]] cc::optional<quadric_hit> intersect(quadric_primitive const& primitive,
                                                  tg::ray3f const& ray,
                                                  float t_min = 0.0f,
                                                  float t_max = 3.4e38f);

/// Appends the primitives of a round-capped segment — the open cylinder, plus a sphere at each end.
///
/// Three records rather than one, because a capsule's surface is not degree 2.
/// Where the joints already carry spheres — a mesh's vertices, above all — `create_cylinder` alone is what to write instead, and
/// the flat cap is exact there rather than an approximation.
void append_capsule(cc::vector<quadric_primitive>& out, tg::segment3f const& s, float radius);
} // namespace sv
