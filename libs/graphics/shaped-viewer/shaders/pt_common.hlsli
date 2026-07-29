#pragma once

#include "camera.hlsli"
#include "light.hlsli"

// Shared state for the path tracer's ray-tracing shaders: the per-frame constants, the ray payload, and the
// sampling helpers the raygen integrator uses.
// FrameConstants mirrors sv::pt_frame_constants_gpu (pathtrace_routine.hh) lane-for-lane — keep them in lockstep.

static const float PT_PI = 3.14159265358979323846;

cbuffer FrameConstants : register(b0)
{
    Camera camera; // pinhole camera basis (see sv::camera_gpu::from)

    AreaLight light; // the single rectangular area light the integrator samples for direct lighting

    // path-tracer controls (accum_frame drives progressive accumulation: 0 restarts, >0 blends in place)
    int  samples_per_pixel;  int max_bounces;  uint rng_seed;  uint accum_frame;

    uint mesh_is_indexed; // 0 => the bound mesh is a plain triangle list; see mesh.hlsli
};

// One path segment's hit result. The raygen (caller) reads it back; the closest-hit fills the surface fields;
// the miss marks the segment escaped (hit_t < 0). SM 6.8 wants the DXR 1.1 [raypayload] annotation with
// per-field read/write stage qualifiers.
// Every field is written by both the closest-hit and the miss (the miss writes zeros plus hit_t < 0), so the
// caller can read them all unconditionally right after TraceRay without the access analyzer flagging an
// undefined read — it then branches on hit_t.
struct [raypayload] PtPayload
{
    float3 albedo   : read(caller) : write(closesthit, miss);
    float3 emissive : read(caller) : write(closesthit, miss);
    float3 normal   : read(caller) : write(closesthit, miss);
    float  hit_t    : read(caller) : write(closesthit, miss); // < 0 => the ray escaped (miss)
};

// A separate, minimal payload for shadow rays. A shadow ray reads only visibility, so giving it its own type
// keeps the payload-access qualifiers exact (the surface payload's fields would otherwise be flagged as
// declared-but-unread on the shadow trace). The caller seeds `visible` to 0 (assume occluded) and the shadow
// miss shader flips it to 1 when the ray reaches the light unobstructed.
struct [raypayload] ShadowPayload
{
    float visible : read(caller) : write(caller, miss);
};

// A hashed per-lane RNG (PCG-style). Seed once per pixel/sample, then pull uniforms in [0, 1).
uint pt_hash(uint x)
{
    x = x * 747796405u + 2891336453u;
    uint w = ((x >> ((x >> 28) + 4u)) ^ x) * 277803737u;
    return (w >> 22) ^ w;
}

float pt_rand(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint w = ((state >> ((state >> 28) + 4u)) ^ state) * 277803737u;
    return float((w >> 22) ^ w) * (1.0 / 4294967296.0);
}

// A cosine-weighted direction in the hemisphere around N (pdf = cos(theta) / PI). u1, u2 are uniforms in [0, 1).
float3 pt_sample_cosine_hemisphere(float3 N, float u1, float u2)
{
    float r = sqrt(u1);
    float phi = 2.0 * PT_PI * u2;
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - u1));

    // an orthonormal basis around N, avoiding the degenerate up when N is near +/-z
    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 t = normalize(cross(up, N));
    float3 b = cross(N, t);
    return normalize(t * x + b * y + N * z);
}
