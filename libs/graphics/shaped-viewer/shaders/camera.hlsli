#pragma once

// Pinhole camera basis (see sv::camera_gpu::from), mirroring sv::camera_gpu (camera.hh) lane-for-lane.
// Each float3 sits in its own 16-byte cbuffer lane (the trailing pad scalar), matching the C++ std140-ish layout.
struct Camera
{
    float3 position;     float _c0;
    float3 forward;      float _c1;
    float3 right_scaled; float _c2; // right * aspect * tan(fov_y / 2)
    float3 up_scaled;    float _c3; // true_up * tan(fov_y / 2)
};

// The primary ray's offset from the camera position, for a continuous pixel position over `dim`.
//
// `pixel` carries whatever sub-pixel jitter the caller drew, so a pixel's centre is its integer coordinate plus 0.5.
// Unnormalized, because `camera_project` inverts exactly this and the length is the view depth it divides by.
float3 camera_ray_offset(Camera cam, float2 pixel, float2 dim)
{
    float2 ndc = pixel / dim * 2.0 - 1.0; // [-1, 1], y down
    return cam.forward + cam.right_scaled * ndc.x - cam.up_scaled * ndc.y;
}

// Where `offset` — a point relative to the camera's position, or a direction for a point at infinity — lands on `cam`'s
// image, as a continuous pixel position over `dim`.
//
// The inverse of `camera_ray_offset`, and a reprojection is only as good as that: the two describe one pinhole, so a
// change to either is a change to both.
// Behind the camera is reported far off the image, which a reprojection reads as "not visible last frame".
float2 camera_project(Camera cam, float3 offset, float2 dim)
{
    float z = dot(offset, normalize(cam.forward));
    if (z <= 1e-6)
        return float2(-1e6, -1e6);
    float2 ndc = float2(dot(offset, cam.right_scaled) / dot(cam.right_scaled, cam.right_scaled),
                        -dot(offset, cam.up_scaled) / dot(cam.up_scaled, cam.up_scaled))
               / z;
    return (ndc + 1.0) * 0.5 * dim;
}
