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
};

// One path segment, in and out.
//
// The closest-hit does the shading: it evaluates the material's BSDF, estimates direct light from it, and samples the
// continuation direction. So the payload carries the segment's RESULT rather than its surface, which is what lets a layered
// BSDF stay on the hit shader's stack instead of being squeezed through here.
//
// `rng` travels the other way, written by the caller. The random state round-trips so the whole path pulls from one stream.
//
// Every result field is written by both the closest-hit and the miss (the miss writes zeros plus hit_t < 0), so the caller can
// read them all unconditionally right after TraceRay without the access analyzer flagging an undefined read; it then branches
// on hit_t. SM 6.8 wants the DXR 1.1 [raypayload] annotation with per-field read/write stage qualifiers.
struct [raypayload] PtPayload
{
    uint rng : read(caller, closesthit) : write(caller, closesthit);

    // The absorption coefficient of the medium the ray is travelling through, round-tripping like `rng`.
    //
    // The caller writes what the CURRENT segment travelled in and the hit writes what the CONTINUATION will, because only
    // the hit knows whether the direction it sampled crossed the surface — and only the caller knows how far the segment it
    // is about to take actually goes.
    // Zero is vacuum, which is every path that never entered a transmissive solid.
    float3 medium : read(caller, closesthit) : write(caller, closesthit);

    float3 direct     : read(caller) : write(closesthit, miss); // next-event estimate at this hit, BSDF folded in
    float3 emission   : read(caller) : write(closesthit, miss); // the surface's own emission, or the sky on a miss
    float3 throughput : read(caller) : write(closesthit, miss); // f * cos / pdf for the sampled continuation
    float3 direction  : read(caller) : write(closesthit, miss); // where the path goes next
    float3 normal     : read(caller) : write(closesthit, miss); // shading normal, for the ray offset off the surface

    float bsdf_pdf : read(caller) : write(closesthit, miss); // pdf of `direction`, for the escaped-environment MIS weight
    float hit_t    : read(caller) : write(closesthit, miss); // < 0 => the ray escaped (miss)
};

// The pdf of the environment sampler the closest-hit estimates with: one uniform-hemisphere direction.
// The raygen needs the same number to weight a bounce ray that escaped, so it lives here rather than in either of them.
static const float PT_ENV_PDF = 1.0 / (2.0 * PT_PI);

// The balance heuristic over two strategies, which is what keeps the sum of the two unbiased.
float pt_mis_weight(float pdf_this, float pdf_other)
{
    return pdf_this / max(pdf_this + pdf_other, 1e-9);
}

// The area light's two estimators — the closest-hit's next-event sample and the raygen's continuation ray reaching the
// rect — balance against each other, so both must form the light's pdf from THIS function.
// A pdf the two disagree about is not a weighting error that shows up as noise; the two weights stop summing to one and
// the estimate is simply wrong.

// The rect's area. Its full edges are 2u and 2v, so the parallelogram is |cross(2u, 2v)| = 4 |cross(u, v)|.
float pt_light_area()
{
    return 4.0 * length(cross(light.u, light.v));
}

// The solid-angle pdf of reaching the light along a direction, given the squared distance to the point reached and the
// cosine at the light's own face — uniform area sampling, reprojected onto the sphere of directions.
float pt_light_pdf(float dist2, float cos_light)
{
    return dist2 / max(pt_light_area() * cos_light, 1e-9);
}

// Where a ray crosses the light's rect, if it does at all.
//
// `t_hit` is the distance along a UNIT `dir` and `cos_light` the cosine at the emitting face; false leaves both zeroed
// and means the ray misses the rect, runs parallel to its plane, or arrives at its back.
//
// The rect spans center +/- u +/- v, and u and v need not be perpendicular — only non-parallel — so the in-plane
// coordinates come from the reciprocal basis rather than from two dot products.
bool pt_light_intersect(float3 origin, float3 dir, out float t_hit, out float cos_light)
{
    t_hit = 0.0;
    cos_light = 0.0;

    float denom = dot(dir, light.normal);
    if (denom >= -1e-9)
        return false; // parallel to the plane, or arriving at the face the light does not emit from

    float t = dot(light.center - origin, light.normal) / denom;
    if (t <= 1e-3)
        return false; // behind the ray, or inside the origin's own offset

    // d = s*u + t*v with both in [-1, 1]; cross(d, v) = s*cross(u, v) and cross(u, d) = t*cross(u, v), which inverts
    // the pair exactly for any non-parallel u and v.
    float3 d = origin + dir * t - light.center;
    float3 n_uv = cross(light.u, light.v);
    float inv = 1.0 / max(dot(n_uv, n_uv), 1e-18);
    float s_uv = dot(cross(d, light.v), n_uv) * inv;
    float t_uv = dot(cross(light.u, d), n_uv) * inv;
    if (abs(s_uv) > 1.0 || abs(t_uv) > 1.0)
        return false;

    t_hit = t;
    cos_light = -denom;
    return true;
}

// Whether every component of `v` is an ordinary finite number.
//
// Written as a magnitude comparison rather than `isnan` / `isinf` on purpose: a NaN compares false against everything,
// so this rejects one either way, and it survives the relaxed float math a compiler may assume where the intrinsics
// can fold to a constant false.
bool pt_is_finite(float3 v)
{
    return all(abs(v) < 1e30);
}

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
