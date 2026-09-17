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
    uint flags;
};

/// The stride of one primitive record: 12 + 40 + 40 + 4.
static const uint quadric_stride = 96;

/// Set in `flags` when the clipper's own surface is drawn too — a cylinder's end caps, a hemisphere's floor.
/// Mirrors `sv::quadric_primitive::flag_emit_clip_surface`.
static const uint quadric_flag_emit_clip_surface = 1u;

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
    p.flags = b.Load(base + 92);
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
    float3 normal; ///< object space, normalized
    bool valid;
};

/// A p transformed by the quadric's symmetric 3x3 block.
float3 quadric_a_mul(quadric3 q, float3 v)
{
    return float3(q.diag.x * v.x + q.off_diag.x * v.y + q.off_diag.y * v.z,
                  q.off_diag.x * v.x + q.diag.y * v.y + q.off_diag.z * v.z,
                  q.off_diag.y * v.x + q.off_diag.z * v.y + q.diag.z * v.z);
}

/// The real roots of `q` along the ray, ascending, plus the point they were solved about; `count` is 0, 1 or 2.
///
/// A vanishing quadratic term is a ray parallel to a degenerate direction of the quadric — a slab or a plane pair — where the
/// equation is linear and has exactly one root, and treating it as a quadratic would divide by zero.
///
/// `t0` / `t1` are along the original ray, so a caller compares and reports them directly.
/// `shifted_origin` and `t_closest` are what the solve actually used, and a caller wanting the HIT POINT must build it from
/// those — `shifted_origin + d * (t - t_closest)` — or it throws away the precision the shift bought.
struct quadric_roots
{
    int count;
    float t0;
    float t1;
    float3 shifted_origin;
    float t_closest;
};

quadric_roots roots_of(quadric3 q, float3 o_in, float3 d)
{
    quadric_roots res; // NOT `out`, which HLSL reserves as a parameter modifier
    res.count = 0;
    res.t0 = 0.0;
    res.t1 = 0.0;
    res.shifted_origin = o_in;
    res.t_closest = 0.0;

    float3 a_d = quadric_a_mul(q, d);
    float qa = dot(d, a_d);

    if (qa == 0.0)
    {
        float qb_linear = 2.0 * (dot(d, quadric_a_mul(q, o_in)) + dot(q.linear_term, d));
        if (qb_linear == 0.0)
            return res;

        res.count = 1;
        res.t0 = -quadric_evaluate(q, o_in) / qb_linear;
        return res;
    }

    // Re-expressed about the ray's closest approach before the roots are formed.
    //
    // A PRECISION REQUIREMENT rather than a simplification, and the CPU reference does the same thing for the same reason.
    // `qc` is Q at whatever origin the solve is expressed about, so for a small primitive seen from far away it is dominated
    // by the distance rather than by the radius: a sphere of radius 0.01 seen from 100 units has a qc of 1e4 - 1e-4, and the
    // ulp of 1e4 in float32 is about 1e-3, so the radius is gone before the discriminant is formed.
    // What is left is a difference of two huge nearly-equal numbers whose SIGN is rounding noise.
    //
    // The per-primitive origin fixes distance from the WORLD origin; this fixes distance from the RAY origin.
    // Exact for any quadric, because shifting the origin along the ray is a reparameterization.
    float qb_in = 2.0 * (dot(d, quadric_a_mul(q, o_in)) + dot(q.linear_term, d));
    float t_closest = -qb_in / (2.0 * qa);
    float3 o = o_in + d * t_closest;

    float qb = 2.0 * (dot(d, quadric_a_mul(q, o)) + dot(q.linear_term, d));
    float qc = quadric_evaluate(q, o);

    res.shifted_origin = o;
    res.t_closest = t_closest;

    float disc = qb * qb - 4.0 * qa * qc;
    if (disc < 0.0)
        return res;

    float root = sqrt(disc);

    // The numerically stable pair: forming both roots from -b - sign(b) sqrt(disc) avoids the cancellation that
    // (-b + sqrt(disc)) suffers when b and sqrt(disc) nearly agree, which is exactly the grazing hit.
    float s = qb >= 0.0 ? -0.5 * (qb + root) : -0.5 * (qb - root);
    float r0 = s / qa;
    float r1 = s == 0.0 ? r0 : qc / s;

    res.count = 2;
    res.t0 = min(r0, r1) + t_closest;
    res.t1 = max(r0, r1) + t_closest;
    return res;
}

/// The nearest point of the SOLID {surface <= 0} AND {clip <= 0} along the ray, within [t_min, t_max].
///
/// Up to four candidates: the two roots of each quadric.
/// Each counts only where it lies inside the OTHER's interior, which is what makes a cylinder's flat cap fall out of the
/// clipper rather than needing geometry of its own — and what makes an open tube and a capped one one record and one bit.
///
/// Mirrors `sv::intersect` (scene/quadric.hh) exactly; a divergence between them is a bug in one rather than a difference of
/// intent.
quadric_result intersect_quadric(quadric_primitive prim, float3 origin, float3 dir, float t_min, float t_max)
{
    quadric_result r;
    r.t = 0.0;
    r.normal = float3(0, 0, 1);
    r.valid = false;

    float3 o = origin - prim.origin;

    quadric_roots surface_roots = roots_of(prim.surface, o, dir);
    bool emit_clip = (prim.flags & quadric_flag_emit_clip_surface) != 0;
    quadric_roots clip_roots;
    clip_roots.count = 0;
    clip_roots.t0 = 0.0;
    clip_roots.t1 = 0.0;
    if (emit_clip)
        clip_roots = roots_of(prim.clip, o, dir);

    // Unrolled over the four candidates rather than looped over two lists, because the shapes are known and a loop here
    // costs a branch per lane.
    // Written with if/else rather than a ternary on the roots: HLSL's conditional operator takes numeric types only, not
    // structs, so selecting a quadric with `?:` does not compile.
    [unroll] for (int i = 0; i < 4; ++i)
    {
        bool on_surface = i < 2;
        int slot = i & 1;

        // The point is built from the SHIFTED origin its own roots were solved about, never from `o` — see `roots_of`.
        // Rebuilding it as `o + dir * t` at a hundred times the primitive's radius throws the precision straight back
        // away, and the clip test and the gradient would then be taken at a point that is not on the surface.
        float t;
        float3 solved_from;
        float t_closest;
        if (on_surface)
        {
            if (surface_roots.count <= slot)
                continue;
            t = slot == 0 ? surface_roots.t0 : surface_roots.t1;
            solved_from = surface_roots.shifted_origin;
            t_closest = surface_roots.t_closest;
        }
        else
        {
            if (clip_roots.count <= slot)
                continue;
            t = slot == 0 ? clip_roots.t0 : clip_roots.t1;
            solved_from = clip_roots.shifted_origin;
            t_closest = clip_roots.t_closest;
        }

        if (t < t_min || t > t_max)
            continue;
        if (r.valid && t >= r.t)
            continue;

        float3 p = solved_from + dir * (t - t_closest);

        // A candidate on one boundary counts only where it lies inside the OTHER's interior.
        float outside;
        float3 normal;
        if (on_surface)
        {
            outside = quadric_evaluate(prim.clip, p);
            normal = quadric_gradient(prim.surface, p);
        }
        else
        {
            outside = quadric_evaluate(prim.surface, p);
            normal = quadric_gradient(prim.clip, p);
        }

        if (outside > 0.0)
            continue;

        r.t = t;
        r.normal = normalize(normal);
        r.valid = true;
    }

    return r;
}

/// The shading context for a hit on a quadric.
///
/// **This is the whole preamble difference between the two geometries.**
/// A quadric numbers its primitives and nothing else, so `primitive` is the one field a generated material can read — which is
/// exactly `per_triangle`, and exactly what the mesh preamble puts there too.
/// The triangle fields are zeroed rather than faked: resolution refuses to source an attribute at a frequency this geometry
/// does not number, so nothing generated for a quadric ever reads them.
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
