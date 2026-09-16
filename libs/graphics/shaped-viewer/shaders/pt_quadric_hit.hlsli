#pragma once

#include "pt_shade.hlsli" // the shading tail every geometry kind shares

// The path tracer's QUADRIC hit group, as the tail of a generated material permutation.
//
// The counterpart of `pt_material_hit.hlsli`, and deliberately the same shape: an epilogue that calls `sv_evaluate_material`,
// which the generated file defines just above it.
// What differs is only how a hit is found and described — an intersection shader solving a quadratic, instead of the hardware
// handing back a triangle and its barycentrics — and everything from there on is `pt_shade`.
//
// A procedural BLAS must be traced by a hit group that HAS an intersection shader, and a triangle BLAS by one that does not.
// That is what makes this a separate group rather than a branch: the choice is the acceleration structure's, not the shader's.

/// What the intersection shader hands its closest-hit.
///
/// The normal rather than nothing, because the intersection has already computed it — the gradient at the hit point falls out of
/// the same solve, and recomputing it in the closest-hit would be the one piece of work this design exists to avoid.
/// Object space; the closest-hit moves it to world.
///
/// 12 bytes, which is why `pathtrace_routine` declares a 12-byte attribute maximum rather than the 8 a triangle's barycentrics
/// need.
struct QuadricAttributes
{
    float3 normal;
};

/// The batch's primitive buffer, reached through the instance.
///
/// `vertices` names it, which is the same field a triangle instance points at its positions with: it means "the geometry buffer
/// this instance reads", and for a quadric batch that is the primitive records.
/// The BOXES the BLAS was built from are a different buffer and are deliberately unreachable here — an intersection shader
/// cannot read its own acceleration structure, which is why the two are not one buffer.
ByteAddressBuffer pt_quadric_buffer(sv::instance inst)
{
    return gBindlessBuffers[NonUniformResourceIndex(inst.vertices)];
}

/// Solves the ray against this primitive and reports the hit the traversal should keep.
///
/// Object space throughout: the batch's primitives live in the set's own frame, and the TLAS instance transform is what places
/// that frame in the world — so `ObjectRayOrigin` / `ObjectRayDirection` are what the quadratic is solved against.
///
/// `RayTCurrent()` is the traversal's current best, so passing it as the upper bound lets a primitive behind an already-found
/// hit reject on the range test rather than on a ReportHit the traversal would discard.
[shader("intersection")]
void QuadricIntersection()
{
    sv::instance inst = pt_bindings::Instances[InstanceID()];
    sv::quadric_primitive prim = sv::load_quadric(pt_quadric_buffer(inst), PrimitiveIndex());

    sv::quadric_result hit
        = sv::intersect_quadric(prim, ObjectRayOrigin(), ObjectRayDirection(), RayTMin(), RayTCurrent());

    if (!hit.valid)
        return;

    QuadricAttributes attribs;
    attribs.normal = hit.normal;
    ReportHit(hit.t, 0u, attribs);
}

[shader("closesthit")]
void PtQuadricClosestHit(inout PtPayload payload, in QuadricAttributes attribs)
{
    sv::instance inst = pt_bindings::Instances[InstanceID()];
    sv::shading_context ctx = sv::make_quadric_context(inst, PrimitiveIndex());

    // The gradient the intersection already computed, moved into world space.
    // Exact for a rigid or uniformly scaled placement; a non-uniform one wants the inverse transpose, which is the same
    // approximation the triangle path takes for its face normal.
    float3 N = normalize(mul((float3x3)ObjectToWorld3x4(), attribs.normal));

    float3 V = -normalize(WorldRayDirection());

    // Which side the ray arrived on, read off the geometry BEFORE the normal is turned to face it.
    // A quadric is genuinely two-sided here rather than by convention: the far-root fallback means an open cylinder's
    // INSIDE wall is a hit the integrator has to shade, and its outward gradient points away from the viewer there.
    bool const exiting = dot(N, V) < 0.0;
    if (exiting)
        N = -N;

    pt_shade(payload, ctx, N, V, exiting);
}
