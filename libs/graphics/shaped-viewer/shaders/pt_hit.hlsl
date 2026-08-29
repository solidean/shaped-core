#include "pt_common.hlsli"
#include "background.hlsli" // background_radiance (the SH environment probe)

// The path tracer's two miss shaders.
//
// There is no closest-hit here: one is GENERATED per material permutation, out of the type's fragment plus
// `pt_material_hit.hlsli`, and the pipeline carries one hit group per permutation.
// A miss has no material to specialize on, which is why these two stay hand-authored and shared by every pipeline.

[shader("miss")]
void PtMiss(inout PtPayload payload)
{
    // Write every field so the caller can read them all unconditionally; hit_t < 0 signals the escape.
    // The environment radiance along the escaped direction rides back in `emission` — the raygen adds
    // `throughput * emission` on escape, so the SH probe lights the scene exactly like a distant emitter.
    payload.direct = float3(0, 0, 0);
    payload.emission = background_radiance(normalize(WorldRayDirection()));
    payload.throughput = float3(0, 0, 0);
    payload.direction = float3(0, 0, 0);
    payload.normal = float3(0, 0, 0);
    payload.bsdf_pdf = 0.0;
    payload.hit_t = -1.0;
}

// Shadow-ray miss (miss index 1): reached only when nothing occluded the ray, so mark it visible.
[shader("miss")]
void PtShadowMiss(inout ShadowPayload payload)
{
    payload.visible = 1.0;
}
