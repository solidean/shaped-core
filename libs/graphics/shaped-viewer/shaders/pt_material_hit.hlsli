#pragma once

#include "pt_shade.hlsli" // the shading tail every geometry kind shares

// The path tracer's closest-hit, as the tail of a GENERATED material permutation.
//
// It is an epilogue rather than a shader of its own: it calls `sv_evaluate_material`, which the generated file defines just above
// it, so HLSL needs it emitted after that definition rather than included at the top.
// `sv::material_shader_options::epilogue_include` is what puts it there.
//
// One of these exists per permutation, and they differ only in the material they call.
// That is what a hit group per permutation buys: the shading is specialized, and everything around it is shared.
//
// What is left HERE is the triangle-specific half: the corner read, the flat face normal, and the cutout test.
// The shading itself is `pt_shade`, which `pt_quadric_hit.hlsli` reaches the same way.
//

/// The acceleration structure the shadow rays are traced against.

struct PtAttributes
{
    float2 bary;
};

/// One object-space position out of the instance's own vertex buffer.
/// Positions are `float3`, tightly packed, which is the layout `sv::mesh_manager` uploads.
float3 pt_instance_position(sv::instance inst, uint vertex)
{
    ByteAddressBuffer positions = gBindlessBuffers[NonUniformResourceIndex(inst.vertices)];
    return asfloat(positions.Load3(vertex * 12));
}
/// The cutout test, run at every intersection a non-opaque instance reports.
///
/// STOCHASTIC rather than a fixed threshold: `geometry_opacity` is a coverage fraction, so accepting the hit that fraction of
/// the time is what makes the estimate converge to a partly-covered surface instead of snapping to a binary mask.
/// The accumulator does the averaging, which is why this costs no blending and no sorting.
///
/// The draw comes from the pixel, the frame's seed and the primitive rather than from the payload: an any-hit that wrote the
/// path's random state would have to be granted access to it, and every ray would then carry a stream whose length depends on
/// how many alpha-tested triangles it happened to graze.
/// Varying with `rng_seed` is what keeps a static view refining rather than settling on one dither pattern.
///
/// Only permutations whose material can actually cut out get one attached — see `material_permutation::can_cut_out` — and
/// only an instance whose `opaque_override` is cleared can invoke it, which `view_renderer` sets from the same flag.
///
/// Written as a plain function with two thin entry points over it, because DXR needs one any-hit per payload TYPE.
/// The raygen's ray and a shadow ray reach different hit records carrying different payloads, an any-hit declares exactly
/// one, and neither of them touches the payload — so this is the entire difference between the two.
bool pt_cutout_rejects(PtAttributes attribs)
{
    sv::instance inst = pt_bindings::Instances[InstanceID()];
    ByteAddressBuffer index_buffer = gBindlessBuffers[NonUniformResourceIndex(inst.indices)];
    sv::shading_context ctx = sv::make_context(inst, index_buffer, PrimitiveIndex(), attribs.bary);

    sv::surface surface = sv_evaluate_material(ctx);
    if (surface.geometry_opacity >= 1.0)
        return false; // fully covered: the common case, and it accepts without drawing anything

    uint2 px = DispatchRaysIndex().xy;
    // The instance is in the hash as well as the primitive: without it two instances sharing one geometry draw the SAME `u`
    // at the same primitive in the same pixel, so stacked identical alpha-tested cards cut out identically instead of
    // independently.
    uint h = pt_hash(px.x + px.y * 65536u + pt_bindings::frame.rng_seed * 9781u + PrimitiveIndex() * 2654435761u
                     + InstanceID() * 2246822519u);
    float u = float(h) * (1.0 / 4294967296.0);

    return surface.geometry_opacity < u;
}

[shader("anyhit")]
void PtAnyHit(inout PtPayload payload, in PtAttributes attribs)
{
    if (pt_cutout_rejects(attribs))
        IgnoreHit();
}

/// The same test for a shadow ray, which carries `ShadowPayload` and so needs an entry point of its own.
/// `pt_occluded` selects this record with `RayContributionToHitGroupIndex` 1.
[shader("anyhit")]
void PtShadowAnyHit(inout ShadowPayload payload, in PtAttributes attribs)
{
    if (pt_cutout_rejects(attribs))
        IgnoreHit();
}
[shader("closesthit")]
void PtClosestHit(inout PtPayload payload, in PtAttributes attribs)
{
    sv::instance inst = pt_bindings::Instances[InstanceID()];
    ByteAddressBuffer index_buffer = gBindlessBuffers[NonUniformResourceIndex(inst.indices)];
    sv::shading_context ctx = sv::make_context(inst, index_buffer, PrimitiveIndex(), attribs.bary);

    // Flat face normal from the triangle's own corners, moved into world space.
    // Read through the instance rather than a global vertex buffer, which is what lets one view hold many meshes.
    float3 v0 = pt_instance_position(inst, ctx.corner.x);
    float3 v1 = pt_instance_position(inst, ctx.corner.y);
    float3 v2 = pt_instance_position(inst, ctx.corner.z);
    float3 n_obj = normalize(cross(v1 - v0, v2 - v0));
    float3 N = normalize(mul((float3x3)ObjectToWorld3x4(), n_obj));

    float3 V = -normalize(WorldRayDirection());

    // Which side the ray arrived on, read off the geometry BEFORE the normal is turned to face it.
    // The shading frame always faces the ray, so this is the only place the distinction survives — and a refraction needs
    // it, since the index ratio inverts between going in and coming back out.
    bool const exiting = dot(N, V) < 0.0;
    if (exiting)
        N = -N; // two-sided: face the incoming ray so arbitrary winding still shades

    pt_shade(payload, ctx, N, V, exiting);
}
