#include "pt_common.hlsli"
#include "background.hlsli" // background_radiance (the SH environment probe)
#include "mesh.hlsli"       // Vertices / Indices + mesh_triangle
#include "pbr.hlsli"        // PbrMaterial (mirrors sv::pbr_material_gpu)

// Miss + closest-hit for the path tracer. The closest-hit reports the surface the path landed on (the flat
// face normal recomputed from the mesh's own vertices, plus the per-primitive material); the miss marks the
// segment as escaped and carries back the environment radiance along the escaped direction. Both are shared by
// the primary/bounce rays; shadow rays skip the closest-hit entirely.

StructuredBuffer<PbrMaterial> Materials : register(t1);

struct Attributes
{
    float2 bary;
};

[shader("miss")]
void PtMiss(inout PtPayload payload)
{
    // Write every field so the caller can read them all unconditionally; hit_t < 0 signals the escape. The
    // environment radiance along the escaped direction rides back in `emissive` — the raygen adds
    // `throughput * emissive` on escape, so the SH probe lights the scene exactly like a distant emitter.
    payload.albedo = float3(0, 0, 0);
    payload.emissive = background_radiance(normalize(WorldRayDirection()));
    payload.normal = float3(0, 0, 0);
    payload.hit_t = -1.0;
}

// Shadow-ray miss (miss index 1): reached only when nothing occluded the ray, so mark it visible.
[shader("miss")]
void PtShadowMiss(inout ShadowPayload payload)
{
    payload.visible = 1.0;
}

[shader("closesthit")]
void PtClosestHit(inout PtPayload payload, in Attributes attribs)
{
    uint prim = PrimitiveIndex();
    PbrMaterial m = Materials[prim];

    // Flat face normal from the triangle's object-space vertices, moved into world space.
    Triangle3 tri = mesh_triangle(prim, mesh_is_indexed != 0);
    float3 n_obj = normalize(cross(tri.v1 - tri.v0, tri.v2 - tri.v0));
    float3 N = normalize(mul((float3x3)ObjectToWorld3x4(), n_obj));

    float3 V = -normalize(WorldRayDirection());
    if (dot(N, V) < 0.0)
        N = -N; // two-sided: face the incoming ray so arbitrary winding still shades

    payload.albedo = m.base_color;
    payload.emissive = m.emissive;
    payload.normal = N;
    payload.hit_t = RayTCurrent();
}
