#pragma once

#include "pt_common.hlsli"

// The path tracer's closest-hit, as the tail of a GENERATED material permutation.
//
// It is an epilogue rather than a shader of its own: it calls `sv_evaluate_material`, which the generated file defines just above
// it, so HLSL needs it emitted after that definition rather than included at the top.
// `sv::material_shader_options::epilogue_include` is what puts it there.
//
// One of these exists per permutation, and they differ only in the material they call.
// That is what a hit group per permutation buys: the shading is specialized, and everything around it — the geometry read, the
// two-sided normal, the payload — is this one file.

/// The per-item table, indexed by `InstanceID()` — mirrors `sv::instance_gpu`.
/// An ordinary binding rather than a bindless one: there is exactly one table, and what varies per instance is what it *points* at.
StructuredBuffer<sv_instance> Instances : register(t4, space0);

struct PtAttributes
{
    float2 bary;
};

/// One object-space position out of the instance's own vertex buffer.
/// Positions are `float3`, tightly packed, which is the layout `sv::mesh_manager` uploads.
float3 pt_instance_position(sv_instance inst, uint vertex)
{
    ByteAddressBuffer positions = gBindlessBuffers[NonUniformResourceIndex(inst.vertices)];
    return asfloat(positions.Load3(vertex * 12));
}

[shader("closesthit")]
void PtClosestHit(inout PtPayload payload, in PtAttributes attribs)
{
    sv_instance inst = Instances[InstanceID()];
    ByteAddressBuffer index_buffer = gBindlessBuffers[NonUniformResourceIndex(inst.indices)];
    sv_shading_context ctx = sv_make_context(inst, index_buffer, PrimitiveIndex(), attribs.bary);

    sv_surface surface = sv_evaluate_material(ctx);

    // Flat face normal from the triangle's own corners, moved into world space.
    // Read through the instance rather than a global vertex buffer, which is what lets one view hold many meshes.
    float3 v0 = pt_instance_position(inst, ctx.corner.x);
    float3 v1 = pt_instance_position(inst, ctx.corner.y);
    float3 v2 = pt_instance_position(inst, ctx.corner.z);
    float3 n_obj = normalize(cross(v1 - v0, v2 - v0));
    float3 N = normalize(mul((float3x3)ObjectToWorld3x4(), n_obj));

    float3 V = -normalize(WorldRayDirection());
    if (dot(N, V) < 0.0)
        N = -N; // two-sided: face the incoming ray so arbitrary winding still shades

    payload.albedo = surface.albedo;
    payload.emissive = surface.emissive;
    payload.normal = N;
    payload.hit_t = RayTCurrent();
}
