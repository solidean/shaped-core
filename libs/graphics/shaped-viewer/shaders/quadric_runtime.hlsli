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

/// The real roots of `q` along the ray, ascending; `count` is 0, 1 or 2.
///
/// A vanishing quadratic term is a ray parallel to a degenerate direction of the quadric — a slab or a plane pair — where the
/// equation is linear and has exactly one root, and treating it as a quadratic would divide by zero.
struct quadric_roots
{
    int count;
    float t0;
    float t1;
};

quadric_roots roots_of(quadric3 q, float3 o, float3 d)
{
    quadric_roots res; // NOT `out`, which HLSL reserves as a parameter modifier
    res.count = 0;
    res.t0 = 0.0;
    res.t1 = 0.0;

    float3 a_o = float3(q.diag.x * o.x + q.off_diag.x * o.y + q.off_diag.y * o.z,
                        q.off_diag.x * o.x + q.diag.y * o.y + q.off_diag.z * o.z,
                        q.off_diag.y * o.x + q.off_diag.z * o.y + q.diag.z * o.z);
    float3 a_d = float3(q.diag.x * d.x + q.off_diag.x * d.y + q.off_diag.y * d.z,
                        q.off_diag.x * d.x + q.diag.y * d.y + q.off_diag.z * d.z,
                        q.off_diag.y * d.x + q.off_diag.z * d.y + q.diag.z * d.z);

    float qa = dot(d, a_d);
    float qb = 2.0 * (dot(d, a_o) + dot(q.linear_term, d));
    float qc = quadric_evaluate(q, o);

    if (qa == 0.0)
    {
        if (qb == 0.0)
            return res;

        res.count = 1;
        res.t0 = -qc / qb;
        return res;
    }

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
    res.t0 = min(r0, r1);
    res.t1 = max(r0, r1);
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

        float t;
        if (on_surface)
        {
            if (surface_roots.count <= slot)
                continue;
            t = slot == 0 ? surface_roots.t0 : surface_roots.t1;
        }
        else
        {
            if (clip_roots.count <= slot)
                continue;
            t = slot == 0 ? clip_roots.t0 : clip_roots.t1;
        }

        if (t < t_min || t > t_max)
            continue;
        if (r.valid && t >= r.t)
            continue;

        float3 p = o + dir * t;

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
