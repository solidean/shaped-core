#include "camera.hlsli"

// A probe over camera.hlsli's projection, so a test can assert on the NUMBERS the raygen's motion guide is built from.
//
// Motion vectors are the one guide nothing downstream can sanity-check: a temporal denoiser fed wrong ones does not
// fail, it smears, and an image that is merely soft looks like a denoiser doing its job.
// Reading them out of a rendered frame cannot separate a wrong vector from a wrong trace either, which is why this
// calls the real `camera_project` and `camera_ray_offset` rather than reimplementing them.
//
// One work item per case, since a projection is a handful of dot products rather than an integral.

namespace sv
{
/// One projection to measure — mirrors `sv_test::camera_probe_case` lane-for-lane, so keep the two in lockstep.
///
/// `world` is the primary hit this pixel's ray found.
/// `at_infinity` replaces it with the ray's own direction, which is what an escaped ray reprojects as — and the reason
/// the sky moves under rotation and not under translation.
struct camera_probe_case
{
    Camera cam;  ///< this frame's camera
    Camera prev; ///< the camera the previous frame was traced from

    float2 pixel; ///< the continuous pixel position the primary ray leaves from
    float2 dim;   ///< the image extent, in pixels

    float3 world;
    uint at_infinity;
};
} // namespace sv

// A group namespace holds declarations and nothing else.
// See shaped-shader-library/docs/binding-preprocessor.md.
#pragma sc group 0
namespace camera_probe_bindings
{
    StructuredBuffer<sv::camera_probe_case> Cases;

    /// `xy` is the motion vector the raygen would write for this case, and `zw` the round-trip of `pixel` through
    /// `camera_ray_offset` and back through `camera_project` — which must come back as `pixel` itself.
    RWStructuredBuffer<float4> Results;
}

[numthreads(64, 1, 1)] void CameraProbe(uint3 tid : SV_DispatchThreadID)
{
    uint item = tid.x;

    uint count = 0u;
    uint stride = 0u;
    camera_probe_bindings::Results.GetDimensions(count, stride);
    if (item >= count)
        return;

    sv::camera_probe_case c = camera_probe_bindings::Cases[item];

    // The same two lines the raygen runs, against the same functions.
    float3 dir = normalize(camera_ray_offset(c.cam, c.pixel, c.dim));
    float3 offset = c.at_infinity != 0u ? dir : c.world - c.prev.position;
    float2 motion = c.pixel - camera_project(c.prev, offset, c.dim);

    float2 round_trip = camera_project(c.cam, camera_ray_offset(c.cam, c.pixel, c.dim), c.dim);

    camera_probe_bindings::Results[item] = float4(motion.x, motion.y, round_trip.x, round_trip.y);
}
