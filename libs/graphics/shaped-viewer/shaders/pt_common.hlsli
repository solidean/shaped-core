#pragma once

#include "background.hlsli" // Background + the SH evaluation the miss and the hits use
#include "camera.hlsli"
#include "instance.hlsli" // sv::instance — the per-item table the group below declares
#include "light.hlsli" // sv::light — the per-light record the group below declares

// Shared state for the path tracer's ray-tracing shaders: the per-frame constants, the ray payload, and the
// sampling helpers the raygen integrator uses.
// FrameConstants mirrors sv::pt_frame_constants_gpu (pathtrace_routine.hh) lane-for-lane — keep them in lockstep.

static const float PT_PI = 3.14159265358979323846;

// How many scattering events a walk takes before Russian roulette starts ending it.
//
// Not from the first event: killing a short walk adds variance to paths that would have finished on their own, and the
// walks this exists for are the long ones. Sixteen is well past what a thin or moderately dense interior needs.
static const int pt_roulette_after = 16;

// The hard ceiling on one walk, which the roulette should reach only for a medium that absorbs nothing at all.
// A guard against a dispatch that never ends rather than a quality control — see the roulette in pathtrace.hlsl.
static const int pt_scatter_cap = 4096;

// The per-frame constants, mirroring sv::pt_frame_constants_gpu (pathtrace_routine.hh) lane-for-lane.
//
// A `struct` plus a `ConstantBuffer` rather than a `cbuffer` block, because a group namespace holds
// declarations: `cbuffer` opens a scope, and the pass refuses one there rather than numbering something whose
// members it would have to hoist.
// Declared out here for the same reason, since the struct is a scope too.
struct FrameConstants
{
    Camera camera; // pinhole camera basis (see sv::camera_gpu::from)

    // path-tracer controls (accum_frame drives progressive accumulation: 0 restarts, >0 blends in place)
    int samples_per_pixel;
    int max_bounces;
    uint rng_seed;
    uint accum_frame;

    // How many lights `Lights` holds, and where each path's run starts in it — indexed by the light_path_* values.
    uint light_count;
    uint3 _pad0;
    uint4 path_offset;
    uint4 path_count;
};

// Every resource this pipeline's stages share, declared once for all of them.
//
// A closest-hit is GENERATED per material permutation and compiled at runtime, so `scene` used to be written
// twice by hand — here and in pt_material_hit.hlsli — with a comment asserting the two matched.
// One declaration is what makes them match; slib's binding pass writes the addresses into each stage.
// See shaped-shader-library/docs/binding-preprocessor.md.
//
// Nothing here has to leave a register free for anyone.
// A material permutation's samplers used to be hand-numbered `s0`.. in space 0, which only held because this
// group declared no sampler — a coupling between two files that nothing enforced.
// They are a group of their own now (`sv::material_sampler_group`), and the pass writes their addresses too.
#pragma sc group 0
namespace pt_bindings
{
    RaytracingAccelerationStructure scene;

    // The view's accumulator: the running mean of every sample this estimate has drawn, read back and blended into.
    //
    // Read-modify-write at the dispatch's OWN pixel, which is what lets one texture do the job of a ping-pong pair.
    // `accum_frame` is the number of frames already folded in, so a frame's weight is 1 / (accum_frame + 1) — the
    // estimate is per view rather than per pixel, and the CPU restarts it by sending 0.
    RWTexture2D<float4> Output;

    /// The per-item table, indexed by `InstanceID()` — mirrors `sv::instance_gpu`.
    /// An ordinary binding rather than a bindless one: there is exactly one table, and what varies per instance is
    /// what it *points* at.
    StructuredBuffer<sv::instance> Instances;

    ConstantBuffer<Background> background;

    // Declared after the four above so their addresses are the ones they always were: the pass runs one counter
    // across register classes, so appending is the one edit to a shared group that moves nothing.
    ConstantBuffer<FrameConstants> frame;

    /// Every light the trace samples, grouped by path — mirrors what sv::pt_light_table builds.
    /// Appended last for the same reason `frame` was.
    StructuredBuffer<sv::light> Lights;
}

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

    // The medium the ray is travelling through, round-tripping like `rng`.
    //
    // The caller writes what the CURRENT segment travelled in and the hit writes what the CONTINUATION will, because only
    // the hit knows whether the direction it sampled crossed the surface — and only the caller knows how far the segment it
    // is about to take actually goes.
    // Zero extinction is vacuum, which is every path that never entered a solid.
    float3 medium_sigma_t : read(caller, closesthit) : write(caller, closesthit);
    float3 medium_albedo  : read(caller, closesthit) : write(caller, closesthit);
    float  medium_g       : read(caller, closesthit) : write(caller, closesthit);

    // Which wavelength this path has been collapsed onto, or 3 while it still carries all three.
    //
    // A dispersive refraction is what collapses it, and it stays collapsed: once the three channels have been bent apart
    // they no longer describe one ray, so nothing downstream may treat them as one again.
    uint channel : read(caller, closesthit) : write(caller, closesthit);

    // Whether this is the path's last surface hit, so no bounce ray will follow it — written by the caller only.
    //
    // Next-event estimation weights its sample against the bounce ray that would otherwise reach the same light, and the
    // two weights sum to one only when both strategies are taken. With no bounce to come, next-event estimation is the
    // only strategy left and takes the full weight — otherwise the path-length limit silently darkens every rect, sun
    // and sky by the share it left to a ray that never flew.
    uint last_bounce : read(closesthit) : write(caller);

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

// A light's two estimators — the closest-hit's next-event sample and the raygen's continuation ray reaching it —
// balance against each other, so both must form the light's density from `pt_light_pdf`.
// A density the two disagree about is not a weighting error that shows up as noise; the two weights stop summing to one
// and the estimate is simply wrong.

// The probability of next-event estimation choosing any one light: uniform over every light the trace holds.
float pt_light_select_pdf()
{
    return 1.0 / float(max(pt_bindings::frame.light_count, 1u));
}

// The solid-angle density of next-event estimation reaching `light` along a direction, given the squared distance to
// the point reached and the cosine at the light's own face.
//
// The light's choice is part of it: uniform area sampling, reprojected onto the sphere of directions, times the
// probability of having picked this light at all.
float pt_light_pdf(sv::light light, float dist2, float cos_light)
{
    return pt_light_select_pdf() * dist2 / max(light.area * cos_light, 1e-9);
}

// The solid-angle density of next-event estimation reaching a distant disc along a direction inside it: uniform over
// the cone the disc subtends, times the probability of having picked this light.
float pt_disc_pdf(sv::light light)
{
    float solid_angle = 2.0 * PT_PI * light.one_minus_cos_angular_radius;
    return pt_light_select_pdf() / max(solid_angle, 1e-20);
}

// Whether the unit `dir` falls inside a distant disc.
//
// Compared as 1 - cos, which for unit vectors is half the squared distance between them, rather than as a dot product:
// a dot product rounds to within a few ulps of 1, which is the whole width of a small disc.
bool pt_in_disc(sv::light light, float3 dir)
{
    float3 off = dir + light.normal; // dir - toward, since the light travels along `normal`
    return 0.5 * dot(off, off) <= light.one_minus_cos_angular_radius;
}

// Where a ray crosses a rect light, if it does at all.
//
// `t_hit` is the distance along a UNIT `dir` and `cos_light` the cosine at the face it reaches; false leaves both zeroed
// and means the ray misses the rect, runs parallel to its plane, or arrives at a face that does not emit.
//
// The rect spans center +/- u +/- v, and u and v need not be perpendicular — only non-parallel — so the in-plane
// coordinates come from the reciprocal basis rather than from two dot products.
bool pt_rect_intersect(sv::light light, float3 origin, float3 dir, out float t_hit, out float cos_light)
{
    t_hit = 0.0;
    cos_light = 0.0;

    float denom = dot(dir, light.normal);
    bool const two_sided = (light.flags & sv::light_flag_two_sided) != 0u;
    if (two_sided ? abs(denom) <= 1e-9 : denom >= -1e-9)
        return false; // parallel to the plane, or arriving at a face the light does not emit from

    float t = dot(light.position - origin, light.normal) / denom;
    if (t <= 1e-3)
        return false; // behind the ray, or inside the origin's own offset

    // d = s*u + t*v with both in [-1, 1]; cross(d, v) = s*cross(u, v) and cross(u, d) = t*cross(u, v), which inverts
    // the pair exactly for any non-parallel u and v.
    float3 d = origin + dir * t - light.position;
    float3 n_uv = cross(light.u, light.v);
    float inv = 1.0 / max(dot(n_uv, n_uv), 1e-18);
    float s_uv = dot(cross(d, light.v), n_uv) * inv;
    float t_uv = dot(cross(light.u, d), n_uv) * inv;
    if (abs(s_uv) > 1.0 || abs(t_uv) > 1.0)
        return false;

    t_hit = t;
    cos_light = abs(denom);
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

// The Henyey-Greenstein phase function at cosine `mu` between the incoming and outgoing directions, normalized over the
// sphere so it doubles as its own pdf.
// `g` is forward at +1 and back at -1; 0 is isotropic and reduces this to 1 / (4 pi).
float pt_hg_phase(float mu, float g)
{
    float g2 = g * g;
    float d = 1.0 + g2 - 2.0 * g * mu;
    return (1.0 - g2) / max(4.0 * PT_PI * d * sqrt(max(d, 1e-9)), 1e-9);
}

// A direction drawn from the Henyey-Greenstein phase function around `w`, whose pdf is `pt_hg_phase` at the cosine between
// them — so a scattering event needs no separate weight.
float3 pt_sample_hg(float3 w, float g, float u1, float u2)
{
    float mu = 0.0;
    if (abs(g) < 1e-3)
    {
        mu = 1.0 - 2.0 * u1; // isotropic: the cosine is uniform
    }
    else
    {
        float t = (1.0 - g * g) / (1.0 - g + 2.0 * g * u1);
        mu = (1.0 + g * g - t * t) / (2.0 * g);
    }

    float sin_theta = sqrt(max(0.0, 1.0 - mu * mu));
    float phi = 2.0 * PT_PI * u2;

    // A basis around w, avoiding the degenerate up when w is near +/-z.
    float3 up = abs(w.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 t1 = normalize(cross(up, w));
    float3 t2 = cross(w, t1);

    return normalize(t1 * (sin_theta * cos(phi)) + t2 * (sin_theta * sin(phi)) + w * mu);
}

// A direction drawn uniformly from the cone around the unit `w` whose half-angle has `1 - cos` of `one_minus_cos_max`
// (pdf = 1 / solid angle).
// Taking 1 - cos rather than the cosine keeps a small cone's width; sin is formed from it for the same reason.
float3 pt_sample_cone(float3 w, float one_minus_cos_max, float u1, float u2)
{
    float one_minus_cos = u1 * one_minus_cos_max;
    float cos_theta = 1.0 - one_minus_cos;
    float sin_theta = sqrt(max(0.0, one_minus_cos * (2.0 - one_minus_cos)));
    float phi = 2.0 * PT_PI * u2;

    float3 up = abs(w.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 t1 = normalize(cross(up, w));
    float3 t2 = cross(w, t1);
    return normalize(t1 * (sin_theta * cos(phi)) + t2 * (sin_theta * sin(phi)) + w * cos_theta);
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
