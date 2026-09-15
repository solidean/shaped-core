#include "quadric.hh"

#include <clean-core/common/utility.hh> // cc::min, cc::max
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
} // namespace

sv::quadric_primitive sv::quadric_primitive::create_sphere(tg::sphere3f const& s)
{
    auto const r = tg::vec3f(s.radius, s.radius, s.radius);
    return {.origin = s.center,
            .surface = quadric3::sphere_about_origin(s.radius),
            .clip = quadric3::everywhere(),
            .bounds = tg::aabb3f(s.center - r, s.center + r)};
}

sv::quadric_primitive sv::quadric_primitive::create_cylinder(tg::segment3f const& s, float radius)
{
    auto const along = s.pos1 - s.pos0;
    auto const len = along.length();

    // No axis to build a cylinder about, so the honest answer is the sphere the degenerate segment describes.
    if (len <= 0.0f)
        return create_sphere(tg::sphere3f(s.pos0, radius));

    auto const axis = along / len;
    auto const mid = s.pos0 + along * 0.5f;

    return {.origin = mid,
            .surface = quadric3::cylinder_about_origin(axis, radius),
            .clip = quadric3::slab_about_origin(axis, len * 0.5f),
            .bounds = box_around(s.pos0, s.pos1, cylinder_extent(axis, radius))};
}

tg::vec3f sv::quadric_primitive::normal_at(tg::pos3f const& p) const
{
    return tg::normalize(surface.gradient(p - origin));
}

cc::optional<sv::quadric_hit> sv::intersect(quadric_primitive const& primitive, tg::ray3f const& ray, float t_min, float t_max)
{
    auto const o = ray.origin - primitive.origin;
    auto const& d = ray.dir;
    auto const& q = primitive.surface;

    // Q(o + t d) = a t² + b t + c, with A the symmetric block and the factor 2 folded in as in `evaluate`.
    auto const a_o = tg::vec3f(q.diag[0] * o[0] + q.off_diag[0] * o[1] + q.off_diag[1] * o[2],
                               q.off_diag[0] * o[0] + q.diag[1] * o[1] + q.off_diag[2] * o[2],
                               q.off_diag[1] * o[0] + q.off_diag[2] * o[1] + q.diag[2] * o[2]);
    auto const a_d = tg::vec3f(q.diag[0] * d[0] + q.off_diag[0] * d[1] + q.off_diag[1] * d[2],
                               q.off_diag[0] * d[0] + q.diag[1] * d[1] + q.off_diag[2] * d[2],
                               q.off_diag[1] * d[0] + q.off_diag[2] * d[1] + q.diag[2] * d[2]);

    float const qa = tg::dot(d, a_d);
    float const qb = 2.0f * (tg::dot(d, a_o) + tg::dot(q.linear, d));
    float const qc = q.evaluate(o);

    float t_near = 0.0f;
    float t_far = 0.0f;

    // A vanishing quadratic term is a ray parallel to a degenerate direction of the quadric — a slab or a plane pair.
    // The equation is then linear and has exactly one root, and treating it as a quadratic would divide by zero.
    if (qa == 0.0f)
    {
        if (qb == 0.0f)
            return {};

        t_near = -qc / qb;
        t_far = t_near;
    }
    else
    {
        float const disc = qb * qb - 4.0f * qa * qc;
        if (disc < 0.0f)
            return {};

        float const root = tg::sqrt(disc);

        // The numerically stable pair: forming both roots from -b - sign(b) sqrt(disc) avoids the cancellation that
        // (-b + sqrt(disc)) suffers when b and sqrt(disc) nearly agree, which is exactly the grazing hit.
        float const s = qb >= 0.0f ? -0.5f * (qb + root) : -0.5f * (qb - root);
        float const r0 = s / qa;
        float const r1 = s == 0.0f ? r0 : qc / s;

        t_near = cc::min(r0, r1);
        t_far = cc::max(r0, r1);
    }

    // The nearest root the range and the clipper both admit, else the far one under the same two tests.
    for (float const t : {t_near, t_far})
    {
        if (t < t_min || t > t_max)
            continue;

        auto const p = ray.origin + d * t;
        if (!primitive.admits(p))
            continue;

        return quadric_hit{.t = t, .normal = primitive.normal_at(p)};
    }

    return {};
}

void sv::append_capsule(cc::vector<quadric_primitive>& out, tg::segment3f const& s, float radius)
{
    out.push_back(quadric_primitive::create_cylinder(s, radius));
    out.push_back(quadric_primitive::create_sphere(tg::sphere3f(s.pos0, radius)));
    out.push_back(quadric_primitive::create_sphere(tg::sphere3f(s.pos1, radius)));
}
