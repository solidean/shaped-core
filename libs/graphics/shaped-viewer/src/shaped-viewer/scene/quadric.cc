#include "quadric.hh"

#include <clean-core/common/utility.hh> // cc::min, cc::max
#include <clean-core/container/fixed_vector.hh>
#include <clean-core/container/vector.hh>
#include <typed-geometry/linalg/vec_ops.hh> // tg::dot, tg::normalize
#include <typed-geometry/scalar/scalar.hh>

namespace
{
/// The half-extent a cylinder of radius `radius` about unit `axis` adds to its endpoints, per world axis.
///
/// Exact rather than conservative: the cylinder's silhouette along axis i is a circle of radius `radius` projected onto the
/// plane normal to i, whose extent is radius * sqrt(1 - axis_i²).
/// An axis-aligned cylinder therefore adds nothing along its own axis, which a bounding sphere per endpoint would have got wrong
/// by a full radius.
tg::vec3f cylinder_extent(tg::vec3f const& axis, float radius)
{
    auto const e = [&](float a) { return radius * tg::sqrt(cc::max(0.0f, 1.0f - a * a)); };
    return tg::vec3f(e(axis[0]), e(axis[1]), e(axis[2]));
}

tg::aabb3f box_around(tg::pos3f const& a, tg::pos3f const& b, tg::vec3f const& extent)
{
    auto const lo
        = tg::pos3f(cc::min(a[0], b[0]) - extent[0], cc::min(a[1], b[1]) - extent[1], cc::min(a[2], b[2]) - extent[2]);
    auto const hi
        = tg::pos3f(cc::max(a[0], b[0]) + extent[0], cc::max(a[1], b[1]) + extent[1], cc::max(a[2], b[2]) + extent[2]);
    return tg::aabb3f(lo, hi);
}

/// The box of the cone's SOLID — the convex hull of the apex and the base disc — given the disc's per-axis half-extent.
/// Exact rather than conservative: every extremum of that hull is either the apex or a point of the disc's silhouette.
tg::aabb3f cone_box(tg::pos3f const& apex, tg::pos3f const& base, tg::vec3f const& extent)
{
    auto const lo = tg::pos3f(cc::min(apex[0], base[0] - extent[0]), cc::min(apex[1], base[1] - extent[1]),
                              cc::min(apex[2], base[2] - extent[2]));
    auto const hi = tg::pos3f(cc::max(apex[0], base[0] + extent[0]), cc::max(apex[1], base[1] + extent[1]),
                              cc::max(apex[2], base[2] + extent[2]));
    return tg::aabb3f(lo, hi);
}
} // namespace

sv::quadric_primitive sv::quadric_primitive::create_sphere(tg::sphere3f const& s)
{
    auto const r = tg::vec3f(s.radius, s.radius, s.radius);
    return {.origin = s.center,
            .surface = quadric3::sphere_about_origin(s.radius),
            .clip = quadric3::everywhere(),
            .bounds = tg::aabb3f(s.center - r, s.center + r)};
}

sv::quadric_primitive sv::quadric_primitive::create_cylinder(tg::segment3f const& s, float radius, bool capped)
{
    auto const along = s.pos1 - s.pos0;
    auto const len = along.length();

    // No axis to build a cylinder about, so the honest answer is the sphere the degenerate segment describes.
    if (len <= 0.0f)
        return create_sphere(tg::sphere3f(s.pos0, radius));

    auto const axis = along / len;
    auto const mid = s.pos0 + along * 0.5f;

    // The box is the capped solid's either way, which is the same as the open tube's: a flat end cap lies in the plane the
    // slab already cuts, so it adds nothing to the extent.
    // That is what lets `capped` be one bit rather than a second geometry.
    return {.origin = mid,
            .surface = quadric3::cylinder_about_origin(axis, radius),
            .clip = quadric3::slab_about_origin(axis, len * 0.5f),
            .flags = capped ? flag_emit_clip_surface : 0u,
            .bounds = box_around(s.pos0, s.pos1, cylinder_extent(axis, radius))};
}

sv::quadric_primitive sv::quadric_primitive::create_cone(tg::segment3f const& base_to_apex, float base_radius, bool capped)
{
    auto const along = base_to_apex.pos0 - base_to_apex.pos1;
    auto const height = along.length();

    // No axis to build a cone about, so the honest answer is the sphere the degenerate segment describes.
    if (height <= 0.0f)
        return create_sphere(tg::sphere3f(base_to_apex.pos0, base_radius));

    // From the apex towards the base, so the slab that keeps [0, height] along it keeps the nappe the base is on and
    // discards the one opening the other way.
    auto const axis = along / height;

    return {.origin = base_to_apex.pos1,
            .surface = quadric3::cone_about_origin(axis, base_radius / height),
            .clip = quadric3::slab(axis, height * 0.5f, height * 0.5f),
            .flags = capped ? flag_emit_clip_surface : 0u,
            .bounds = cone_box(base_to_apex.pos1, base_to_apex.pos0, cylinder_extent(axis, base_radius))};
}

tg::vec3f sv::quadric_primitive::normal_at(tg::pos3f const& p) const
{
    return tg::normalize(surface.gradient(p - origin));
}

namespace
{
/// The real roots of `q` along the ray, in ascending order.
///
/// `count` is 0, 1 or 2; a vanishing quadratic term is a ray parallel to a degenerate direction of the quadric — a slab or a
/// plane pair — where the equation is linear and has exactly one root, and treating it as a quadratic would divide by zero.
struct quadric_roots
{
    int count = 0;
    float t[2] = {0.0f, 0.0f};
};

[[nodiscard]] quadric_roots roots_of(sv::quadric3 const& q, tg::vec3f const& o, tg::vec3f const& d)
{
    auto const a_o = tg::vec3f(q.diag[0] * o[0] + q.off_diag[0] * o[1] + q.off_diag[1] * o[2],
                               q.off_diag[0] * o[0] + q.diag[1] * o[1] + q.off_diag[2] * o[2],
                               q.off_diag[1] * o[0] + q.off_diag[2] * o[1] + q.diag[2] * o[2]);
    auto const a_d = tg::vec3f(q.diag[0] * d[0] + q.off_diag[0] * d[1] + q.off_diag[1] * d[2],
                               q.off_diag[0] * d[0] + q.diag[1] * d[1] + q.off_diag[2] * d[2],
                               q.off_diag[1] * d[0] + q.off_diag[2] * d[1] + q.diag[2] * d[2]);

    float const qa = tg::dot(d, a_d);
    float const qb = 2.0f * (tg::dot(d, a_o) + tg::dot(q.linear, d));
    float const qc = q.evaluate(o);

    auto out = quadric_roots{};

    if (qa == 0.0f)
    {
        if (qb == 0.0f)
            return out;

        out.count = 1;
        out.t[0] = -qc / qb;
        return out;
    }

    float const disc = qb * qb - 4.0f * qa * qc;
    if (disc < 0.0f)
        return out;

    float const root = tg::sqrt(disc);

    // The numerically stable pair: forming both roots from -b - sign(b) sqrt(disc) avoids the cancellation that
    // (-b + sqrt(disc)) suffers when b and sqrt(disc) nearly agree, which is exactly the grazing hit.
    float const s = qb >= 0.0f ? -0.5f * (qb + root) : -0.5f * (qb - root);
    float const r0 = s / qa;
    float const r1 = s == 0.0f ? r0 : qc / s;

    out.count = 2;
    out.t[0] = cc::min(r0, r1);
    out.t[1] = cc::max(r0, r1);
    return out;
}
} // namespace

cc::optional<sv::quadric_hit> sv::intersect(quadric_primitive const& primitive, tg::ray3f const& ray, float t_min, float t_max)
{
    auto const o = ray.origin - primitive.origin;
    auto const& d = ray.dir;

    auto best = cc::optional<quadric_hit>();

    // The solid is the intersection of the two interiors, so a candidate on either boundary counts only where it lies inside
    // the OTHER — which is the whole test, and the reason the caps of a cylinder need no geometry of their own.
    auto const consider = [&](float t, quadric3 const& own, quadric3 const& other)
    {
        if (t < t_min || t > t_max)
            return;
        if (best.has_value() && t >= best.value().t)
            return;

        auto const p = o + d * t;
        if (other.evaluate(p) > 0.0f)
            return;

        best = quadric_hit{.t = t, .normal = tg::normalize(own.gradient(p))};
    };

    auto const surface_roots = roots_of(primitive.surface, o, d);
    for (auto i = 0; i < surface_roots.count; ++i)
        consider(surface_roots.t[i], primitive.surface, primitive.clip);

    if (primitive.emits_clip_surface())
    {
        auto const clip_roots = roots_of(primitive.clip, o, d);
        for (auto i = 0; i < clip_roots.count; ++i)
            consider(clip_roots.t[i], primitive.clip, primitive.surface);
    }

    return best;
}

cc::fixed_vector<sv::quadric_primitive, 2> sv::arrow_primitives(tg::segment3f const& s, arrow_style const& style)
{
    auto out = cc::fixed_vector<quadric_primitive, 2>();

    auto const along = s.pos1 - s.pos0;
    auto const len = along.length();

    // Nothing points anywhere, so there is no arrow to draw rather than a headless stub to draw instead.
    if (len <= 0.0f)
        return out;

    auto const axis = along / len;
    auto const head_length = cc::min(style.head_length, len);
    auto const head_base = s.pos1 - axis * head_length;

    // The shaft is capped because its far end is only hidden while the head is the wider of the two, which a caller setting
    // the three lengths itself is free to break — an open tube would then show its interior down the whole arrow.
    if (head_length < len)
        out.push_back(quadric_primitive::create_cylinder(tg::segment3f(s.pos0, head_base), style.shaft_radius, true));

    if (head_length > 0.0f)
        out.push_back(quadric_primitive::create_cone(tg::segment3f(head_base, s.pos1), style.head_radius));

    return out;
}

cc::fixed_vector<sv::quadric_primitive, 2> sv::arrow_primitives(tg::segment3f const& s, float shaft_radius)
{
    return arrow_primitives(s, arrow_style::for_shaft_radius(shaft_radius));
}

cc::fixed_vector<sv::quadric_primitive, 2> sv::arrow_primitives(tg::segment3f const& s)
{
    return arrow_primitives(s, arrow_style::for_length((s.pos1 - s.pos0).length()));
}

void sv::append_capsule(cc::vector<quadric_primitive>& out, tg::segment3f const& s, float radius)
{
    out.push_back(quadric_primitive::create_cylinder(s, radius));
    out.push_back(quadric_primitive::create_sphere(tg::sphere3f(s.pos0, radius)));
    out.push_back(quadric_primitive::create_sphere(tg::sphere3f(s.pos1, radius)));
}
