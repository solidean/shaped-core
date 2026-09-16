#pragma once

#include <clean-core/container/fixed_vector.hh>
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

    /// The double cone about the line through the origin along unit `axis`, gaining `slope` of radius per unit of height:
    /// |p|² - (1 + slope²)(p·axis)², whose A is I - (1 + slope²) axis axisᵀ.
    ///
    /// **Both nappes**, because a quadric cannot tell them apart: the equation is even in p·axis, so the cone opening the other
    /// way satisfies it too, and a slab clipper on one side of the apex is what keeps a single one.
    /// The apex is AT the origin, which is why `create_cone` puts the primitive's origin there.
    ///
    /// `axis` must be unit length.
    [[nodiscard]] static constexpr quadric3 cone_about_origin(tg::vec3f const& axis, float slope)
    {
        float const k = 1.0f + slope * slope;
        return {
            .diag = tg::vec3f(1.0f - k * axis[0] * axis[0], 1.0f - k * axis[1] * axis[1], 1.0f - k * axis[2] * axis[2]),
            .off_diag = tg::vec3f(-k * axis[0] * axis[1], -k * axis[0] * axis[2], -k * axis[1] * axis[2])};
    }

    /// The slab of half-height `half_height` centred at `offset` along unit `axis`: (p·axis - offset)² - half_height².
    ///
    /// This is the clipper that turns an infinite cylinder into a finite one, and its A is axis axisᵀ.
    /// Its own zero set is the pair of planes at the slab's ends, which is what a capped cylinder's flat faces ARE — see
    /// `quadric_primitive::emits_clip_surface`.
    ///
    /// `axis` must be unit length.
    [[nodiscard]] static constexpr quadric3 slab(tg::vec3f const& axis, float offset, float half_height)
    {
        return {.diag = tg::vec3f(axis[0] * axis[0], axis[1] * axis[1], axis[2] * axis[2]),
                .off_diag = tg::vec3f(axis[0] * axis[1], axis[0] * axis[2], axis[1] * axis[2]),
                .linear = -offset * axis,
                .constant = offset * offset - half_height * half_height};
    }

    /// The slab of half-height `half_height` about the origin, normal to unit `axis`.
    /// Symmetric, so it carries no sign — see `slab`.
    [[nodiscard]] static constexpr quadric3 slab_about_origin(tg::vec3f const& axis, float half_height)
    {
        return slab(axis, 0.0f, half_height);
    }

    [[nodiscard]] friend constexpr bool operator==(quadric3 const&, quadric3 const&) = default;
};

/// One drawn primitive: two quadrics whose interiors intersect, and the object-space box that bounds the result.
///
/// **The solid is {surface <= 0} AND {clip <= 0}**, and its boundary therefore has two parts.
/// Where `surface == 0` inside the clip region, the surface quadric is what is drawn.
/// Where `clip == 0` inside the surface region, the CLIPPER is — a cylinder's flat end caps, a hemisphere's floor — and
/// `emits_clip_surface` is what says whether that half is drawn at all.
///
/// A ray therefore meets up to FOUR candidate points: two roots of each quadric.
/// Each is kept only if it lies in the other's interior, and the nearest survivor is the hit — which is what makes an
/// open tube and a capped one the same record with one bit different.
///
/// The clipper being a full quadric rather than a plane is what closes this under the shapes that matter.
/// A slab is itself a quadric, so a finite cylinder is a cylinder clipped by a slab, a cone frustum is a cone clipped by a slab,
/// and a hemisphere is a sphere clipped by a plane pair — all in one record and one intersection routine.
///
/// **Everything here is in the owning set's space, never the world's** — `origin`, the two quadrics, and `bounds` alike.
/// `sv::quadric_set::transform` is what places that space in the world, so a caller holding a world position converts it before
/// asking anything below.
/// `bounds` is what the procedural BLAS is built from, and it is computed by the factory that knows the shape rather than
/// derived from the coefficients, because a general quadric has no finite box at all and only the construction knows which
/// bounded shape was meant.
struct sv::quadric_primitive
{
    /// Set in `flags` to draw the clipper's own surface as well as the surface quadric's.
    static constexpr u32 flag_emit_clip_surface = 1u;

    tg::pos3f origin = {};
    sv::quadric3 surface = {};
    sv::quadric3 clip = sv::quadric3::everywhere();

    /// Bitfield over the `flag_` constants above; part of the GPU record, so it is a `u32` rather than a bool.
    u32 flags = 0;

    /// The box the primitive is traced through, in the set's own space.
    ///
    /// **It bounds the SOLID and never the visible part of it.**
    /// Turning `flag_emit_clip_surface` on or off changes which pixels are drawn and must not change this by a single bit:
    /// the box is the acceleration structure's, and a box that tracked visibility would make the same geometry two
    /// resources and a dropped hit a silent hole.
    tg::aabb3f bounds = {};

    [[nodiscard]] bool emits_clip_surface() const { return (flags & flag_emit_clip_surface) != 0; }

    /// The sphere `s`, unclipped.
    [[nodiscard]] static quadric_primitive create_sphere(tg::sphere3f const& s);

    /// The segment `s` thickened by `radius`, as a cylinder clipped to the segment's own slab.
    ///
    /// `capped` decides whether the slab's two end planes are drawn: false is an OPEN tube, which is what a wireframe wants
    /// since a vertex sphere already covers each joint, and true is a closed solid.
    /// The box is the same either way — see `bounds`.
    ///
    /// A round-capped edge is the open tube plus a sphere at each end, which `append_capsule` writes.
    /// A degenerate segment — both endpoints equal — yields a sphere instead, since there is no axis to build a cylinder about.
    [[nodiscard]] static quadric_primitive create_cylinder(tg::segment3f const& s, float radius, bool capped = false);

    /// The cone whose base disc is the circle of radius `base_radius` about `base_to_apex.pos0` and whose tip is `pos1`.
    ///
    /// A cone clipped to the slab between the apex and the base, so one primitive rather than a surface plus a disc.
    /// The slab's far plane passes through the apex, where it meets the cone in that single point alone — so `capped` draws the
    /// base disc and nothing else, and an uncapped cone shows its own hollow interior.
    ///
    /// A degenerate segment — both endpoints equal — yields a sphere, since there is no axis to build a cone about.
    [[nodiscard]] static quadric_primitive create_cone(tg::segment3f const& base_to_apex,
                                                       float base_radius,
                                                       bool capped = true);

    /// Whether `p` lies in the region the clipper admits.
    /// `p` is in the SET's space, like everything else on this type — see the note above the struct.
    [[nodiscard]] bool admits(tg::pos3f const& p) const { return clip.evaluate(p - origin) <= 0.0f; }

    /// The outward unit normal at `p`, a point of the surface in the SET's space.
    /// Undefined where the gradient vanishes, which for the shapes built here is only a degenerate primitive.
    [[nodiscard]] tg::vec3f normal_at(tg::pos3f const& p) const;
};

namespace sv
{
// A set's content hash is taken over the raw bytes of its primitives, so a padding hole would feed indeterminate bytes into a
// cache key — two identical sets could then hash differently and upload twice.
// Every member is 4-byte aligned and the total is their exact sum, so there is no hole; this is what says so out loud.
static_assert(sizeof(quadric_primitive) == 12 + 40 + 40 + 4 + 24,
              "quadric_primitive must stay padding-free — see quadric_set::add");
static_assert(sizeof(quadric3) == 40, "quadric3 must stay padding-free");
} // namespace sv

/// What a ray hit on a quadric primitive is: how far along, and the outward unit normal there.
struct sv::quadric_hit
{
    float t = 0.0f;
    tg::vec3f normal = {};
};

namespace sv
{
/// The default upper bound on `intersect`, meaning "as far as the ray goes".
///
/// The largest finite f32 rather than an infinity, so a caller comparing against it never has to reason about NaN.
/// tg has no scalar bound constant yet — `scalar_traits` carries capabilities and operations but no max value — so this is a
/// local stand-in for a `tg::max_value<f32>` that belongs there.
inline constexpr float unbounded_ray_t = 3.402823466e38f;

/// The CPU reference for what the intersection shader reports, and the shape the two are tested against each other by.
///
/// Reports the nearest of up to FOUR candidates — two roots of each quadric — keeping each only where it lies inside the other
/// quadric's interior and within [`t_min`, `t_max`]; the clipper's own two are candidates only where `emits_clip_surface`.
/// The far-root fallback is load-bearing for ordinary geometry rather than only for interior views: a slab-clipped cylinder is an
/// open tube, so seen near end-on its near root lies outside the slab and the visible surface is the inside of the far wall.
///
/// `ray.dir` need not be normalized; `t` is in units of `ray.dir`, as `TraceRay` reports it.
[[nodiscard]] cc::optional<quadric_hit> intersect(quadric_primitive const& primitive,
                                                  tg::ray3f const& ray,
                                                  float t_min = 0.0f,
                                                  float t_max = unbounded_ray_t);

} // namespace sv

/// How a drawn segment is closed at its two ends.
///
/// The choice is one bit on the primitive for two of the three, and a different primitive COUNT for the third: a capsule's
/// surface is not degree 2, so round ends are the tube plus a sphere at each end.
enum class sv::line_ends : sv::u8
{
    round, ///< a hemisphere at each end — three primitives, and what a standalone polyline wants
    flat,  ///< the clipper's own two planes, drawn — one primitive, a closed solid
    open,  ///< nothing; the tube is open at both ends — one primitive, and what a wireframe wants
};

/// How a line is drawn: how thick, and how its ends are closed.
///
/// The counterpart of `sv::arrow_style`, and absolute in the same way — `radius` is in the set's own units, never relative
/// to the segment's length.
///
/// **The default radius is a starting point rather than a derived value.**
/// An arrow's default lengths come from `for_length(1)`, because an arrow has a length to take them from; a line's thickness
/// is not derivable from anything the type carries, so this is simply a thin line at unit scale and most callers state one.
/// `open` rather than `round` as the default end, because an open tube is one primitive against three and is what a
/// wireframe wants — a caller drawing a standalone polyline asks for `round`.
struct sv::line_style
{
    float radius = 0.01f;
    line_ends ends = line_ends::open;

    [[nodiscard]] friend constexpr bool operator==(line_style const&, line_style const&) = default;
};

/// The three lengths an arrow is drawn from, in the set's own units — absolute, never relative to the arrow.
///
/// Absolute is what a gizmo or a vector field wants: every arrow the same thickness whatever it measures, so that length is
/// the only thing its size encodes.
/// The proportional reading is had from `for_length`, and the overloads of `append_arrow` and `quadric_set::add_arrow` that
/// take no style are that call.
///
/// The defaults are `for_length(1)`, so `arrow_style{}` is the arrow a unit segment wants.
struct sv::arrow_style
{
    /// The shaft radius a proportional arrow gets — 2% of its length, so the shaft reads as a line rather than as a rod.
    static constexpr float shaft_radius_fraction = 0.02f;

    /// The head's radius as a multiple of the shaft's, and its length as a multiple of its radius.
    /// Together they fix the tip's half-angle at atan(1/3) ≈ 18°, which is what makes an arrow read as one at any size.
    static constexpr float head_radius_ratio = 2.5f;
    static constexpr float head_length_ratio = 3.0f;

    // Spelled through the ratios rather than as 0.02 / 0.05 / 0.15, so that `arrow_style{}` and `for_length(1)` agree to the
    // BIT — a literal would differ by an ulp and make two arrows that should share a batch hash to different ones.
    float shaft_radius = shaft_radius_fraction;
    float head_radius = head_radius_ratio * shaft_radius_fraction;
    float head_length = head_length_ratio * (head_radius_ratio * shaft_radius_fraction);

    /// The style whose shaft is `shaft_radius`, with the head scaled to it.
    /// This is what to write for arrows that must all look alike while measuring different lengths.
    [[nodiscard]] static constexpr arrow_style for_shaft_radius(float shaft_radius)
    {
        auto const head_radius = head_radius_ratio * shaft_radius;
        return {.shaft_radius = shaft_radius, .head_radius = head_radius, .head_length = head_length_ratio * head_radius};
    }

    /// The style an arrow of length `length` gets when nothing else is said.
    [[nodiscard]] static constexpr arrow_style for_length(float length)
    {
        return for_shaft_radius(shaft_radius_fraction * length);
    }

    [[nodiscard]] friend constexpr bool operator==(arrow_style const&, arrow_style const&) = default;
};

namespace sv
{
/// The primitives of an arrow from `s.pos0` to `s.pos1`: a capped cylinder for the shaft, and a cone for the head.
///
/// **The tip is exactly `s.pos1` and the tail exactly `s.pos0`** — the head is taken OUT of the segment rather than added past
/// its end, so an arrow drawn between two points measures the distance between them.
/// A head at least as long as the arrow is clamped to it and the shaft is dropped, which is one primitive rather than an
/// inside-out cylinder; a segment with no length yields nothing, since there is no direction for a head to point.
///
/// Returned rather than appended, because two is the most an arrow is and a caller that wants them in a batch has
/// `quadric_set::add_arrow` instead.
[[nodiscard]] cc::fixed_vector<quadric_primitive, 2> arrow_primitives(tg::segment3f const& s, arrow_style const& style);

/// The same with the head scaled to a given shaft radius — `arrow_style::for_shaft_radius`.
[[nodiscard]] cc::fixed_vector<quadric_primitive, 2> arrow_primitives(tg::segment3f const& s, float shaft_radius);

/// The same with every length proportional to the arrow's own — `arrow_style::for_length`.
[[nodiscard]] cc::fixed_vector<quadric_primitive, 2> arrow_primitives(tg::segment3f const& s);

/// The primitives of a round-capped segment — the open cylinder, plus a sphere at each end.
///
/// Three records rather than one, because a capsule's surface is not degree 2.
/// Where the joints already carry spheres — a mesh's vertices, above all — `create_cylinder` alone is what to write instead, and
/// the flat cap is exact there rather than an approximation.
///
/// Returned rather than appended, for the same reason `arrow_primitives` is: three is the most a capsule is, and a caller that
/// wants them in a batch has `quadric_set::add_capsule` instead.
[[nodiscard]] cc::fixed_vector<quadric_primitive, 3> capsule_primitives(tg::segment3f const& s, float radius);

/// The primitives of a line drawn in `style` — one for `flat` and `open`, three for `round`.
[[nodiscard]] cc::fixed_vector<quadric_primitive, 3> line_primitives(tg::segment3f const& s, line_style const& style);
} // namespace sv
