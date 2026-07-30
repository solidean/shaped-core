#pragma once

// Pinhole camera basis (see sv::camera_gpu::from), mirroring sv::camera_gpu (camera.hh) lane-for-lane.
// Each float3 sits in its own 16-byte cbuffer lane (the trailing pad scalar), matching the C++ std140-ish layout.
// The raygen forms a primary ray as `forward + right_scaled * ndc.x - up_scaled * ndc.y`.
struct Camera
{
    float3 position;     float _c0;
    float3 forward;      float _c1;
    float3 right_scaled; float _c2; // right * aspect * tan(fov_y / 2)
    float3 up_scaled;    float _c3; // true_up * tan(fov_y / 2)
};
