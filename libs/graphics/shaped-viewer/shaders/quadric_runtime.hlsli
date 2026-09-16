#pragma once

#include "material_runtime.hlsli" // the contract a generated material is written against; a quadric adds to it rather than forking it

// The quadric half of what a generated material shader is compiled against.
//
// A quadric permutation is the SAME generated material as a triangle one, spelled against this runtime instead of
// `material_runtime.hlsli` — which `sv::material_shader_key` already distinguishes, so the two live side by side in one cache.
// So everything a material fragment reads — `sv::surface`, `sv::shading_context`, the element loads — comes from the include
// above, and what is here is only what a quadric has and a triangle does not.
//
// The geometry is analytic: no vertices, no corners, no barycentrics.
// A ray meets a quadric where a quadratic in the ray parameter is zero, which is the whole reason these are cheap.

namespace sv
{
/// One quadric surface: the 10 distinct entries of the symmetric 4x4 Q, as `sv::quadric3` holds them.
///
/// The surface is the zero set of Q(p) = pᵀ A p + 2 bᵀ p + c, with `diag` and `off_diag` the symmetric 3x3 block, `linear` the
/// b term and `constant` the c.
/// The factor 2 is folded into the evaluation rather than into the storage.
struct quadric3
{
    float3 diag;
    float3 off_diag;
    float3 linear_term; ///< b — NOT `linear`, which HLSL reserves as an interpolation modifier
    float constant;
};

/// One drawn primitive as the intersection shader reads it — mirrors `sv::quadric_gpu` (resources/quadric_data.hh).
///
/// Keep the two in lockstep: it is a byte layout, not a description of one.
/// `origin` is what both quadrics are expressed about, and it is there for precision rather than convenience — a world-space
/// quadric loses a small radius to float32 entirely at mesh scale.
struct quadric_primitive
{
    float3 origin;
    quadric3 surface;
    quadric3 clip;
};

/// The stride of one primitive record: 12 + 40 + 40.
static const uint quadric_stride = 92;

quadric3 load_quadric3(ByteAddressBuffer b, uint offset)
{
    quadric3 q;
    q.diag = asfloat(b.Load3(offset + 0));
    q.off_diag = asfloat(b.Load3(offset + 12));
    q.linear_term = asfloat(b.Load3(offset + 24));
    q.constant = asfloat(b.Load(offset + 36));
    return q;
}

/// The primitive at `index` out of the batch's own primitive buffer.
quadric_primitive load_quadric(ByteAddressBuffer b, uint index)
{
    uint base = index * quadric_stride;

    quadric_primitive p;
    p.origin = asfloat(b.Load3(base));
    p.surface = load_quadric3(b, base + 12);
    p.clip = load_quadric3(b, base + 52);
    return p;
}

/// Q(p), with `p` the displacement from the primitive's origin.
/// Negative is inside for every quadric sv builds, which is what makes a clipper's test a single sign check.
float quadric_evaluate(quadric3 q, float3 p)
{
    return q.diag.x * p.x * p.x + q.diag.y * p.y * p.y + q.diag.z * p.z * p.z
           + 2.0 * (q.off_diag.x * p.x * p.y + q.off_diag.y * p.x * p.z + q.off_diag.z * p.y * p.z)
           + 2.0 * dot(q.linear_term, p) + q.constant;
}

/// ∇Q(p) = 2 (A p + b), which is the outward surface normal wherever it does not vanish.
float3 quadric_gradient(quadric3 q, float3 p)
{
    return 2.0
           * float3(q.diag.x * p.x + q.off_diag.x * p.y + q.off_diag.y * p.z + q.linear_term.x,
                    q.off_diag.x * p.x + q.diag.y * p.y + q.off_diag.z * p.z + q.linear_term.y,
                    q.off_diag.y * p.x + q.off_diag.z * p.y + q.diag.z * p.z + q.linear_term.z);
}

/// What a ray hit on a quadric is: how far along, the outward normal there, and whether there was one at all.
struct quadric_result
{
    float t;
    float3 normal; ///< object space, unnormalized-safe (already normalized here)
    bool valid;
};

/// The nearest root at or beyond `t_min` that the clipper admits, else the far root under the same two tests.
///
/// **The far-root fallback is load-bearing for ordinary geometry**, not only for a ray that starts inside.
/// A slab-clipped cylinder is an OPEN tube, so seen near end-on its near root lies outside the slab and the visible surface is
/// the inside of the far wall — which is what every edge pointing at the camera does.
///
/// Mirrors `sv::intersect` (scene/quadric.hh) exactly, and that is deliberate: the CPU one is the reference this is tested
/// against, so a divergence between them is a bug in one of the two rather than a difference of intent.
quadric_result intersect_quadric(quadric_primitive prim, float3 origin, float3 dir, float t_min, float t_max)
{
    quadric_result r;
    r.t = 0.0;
    r.normal = float3(0, 0, 1);
    r.valid = false;

    float3 o = origin - prim.origin;
    quadric3 q = prim.surface;

    float3 a_o = float3(q.diag.x * o.x + q.off_diag.x * o.y + q.off_diag.y * o.z,
                        q.off_diag.x * o.x + q.diag.y * o.y + q.off_diag.z * o.z,
                        q.off_diag.y * o.x + q.off_diag.z * o.y + q.diag.z * o.z);
    float3 a_d = float3(q.diag.x * dir.x + q.off_diag.x * dir.y + q.off_diag.y * dir.z,
                        q.off_diag.x * dir.x + q.diag.y * dir.y + q.off_diag.z * dir.z,
                        q.off_diag.y * dir.x + q.off_diag.z * dir.y + q.diag.z * dir.z);

    float qa = dot(dir, a_d);
    float qb = 2.0 * (dot(dir, a_o) + dot(q.linear_term, dir));
    float qc = quadric_evaluate(q, o);

    float t_near = 0.0;
    float t_far = 0.0;

    // A vanishing quadratic term is a ray parallel to a degenerate direction of the quadric — a slab or a plane pair.
    // The equation is then linear and has exactly one root, and treating it as a quadratic would divide by zero.
    if (qa == 0.0)
    {
        if (qb == 0.0)
            return r;

        t_near = -qc / qb;
        t_far = t_near;
    }
    else
    {
        float disc = qb * qb - 4.0 * qa * qc;
        if (disc < 0.0)
            return r;

        float root = sqrt(disc);

        // The numerically stable pair: forming both roots from -b - sign(b) sqrt(disc) avoids the cancellation that
        // (-b + sqrt(disc)) suffers when b and sqrt(disc) nearly agree, which is exactly the grazing hit.
        float s = qb >= 0.0 ? -0.5 * (qb + root) : -0.5 * (qb - root);
        float r0 = s / qa;
        float r1 = s == 0.0 ? r0 : qc / s;

        t_near = min(r0, r1);
        t_far = max(r0, r1);
    }

    // Unrolled rather than looped, because the two candidates are known and a loop here costs a branch per lane.
    [unroll] for (int i = 0; i < 2; ++i)
    {
        float t = i == 0 ? t_near : t_far;
        if (r.valid || t < t_min || t > t_max)
            continue;

        float3 p = o + dir * t;
        if (quadric_evaluate(prim.clip, p) > 0.0)
            continue;

        r.t = t;
        r.normal = normalize(quadric_gradient(q, p));
        r.valid = true;
    }

    return r;
}

/// The shading context for a hit on a quadric.
///
/// The three triangle fields are stubbed: a quadric has no corners and no barycentrics, so a `per_vertex` or `per_corner`
/// attribute has nothing to read and is not resolvable onto one.
/// Giving quadrics their own frequencies — `per_quadric` and `per_quadric_end`, the latter blended by the clip slab's own
/// parameter — is the next step, and is what makes this stub go away.
shading_context make_quadric_context(instance inst, uint primitive)
{
    shading_context ctx;
    ctx.param_buffer = inst.param_buffer;
    ctx.param_offset = inst.param_offset;
    ctx.primitive = primitive;
    ctx.corner = uint3(0, 0, 0);
    ctx.barycentrics = float3(1, 0, 0);
    return ctx;
}
} // namespace sv
