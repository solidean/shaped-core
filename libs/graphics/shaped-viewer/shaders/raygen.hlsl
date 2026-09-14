#include "common.hlsli"

// Primary-ray generation: a pinhole camera shoots one ray per pixel and writes the traced color out.
// The camera basis (forward/right/up, pre-scaled by aspect and fov) rides in FrameConstants.

[shader("raygeneration")]
void RayGen()
{
    uint2 px = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;

    float2 ndc = (float2(px) + 0.5) / float2(dim) * 2.0 - 1.0; // [-1, 1], y down
    Camera const cam = flat_bindings::frame.camera;
    float3 dir = normalize(cam.forward + cam.right_scaled * ndc.x - cam.up_scaled * ndc.y);

    RayDesc ray;
    ray.Origin = cam.position;
    ray.Direction = dir;
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    // No init: every ray either hits (closest-hit writes color) or misses (miss writes the sky), so the
    // caller only reads the payload back — matching the read/write qualifiers in common.hlsli.
    Payload payload;
    TraceRay(flat_bindings::scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    flat_bindings::Output[px] = payload.color;
}
