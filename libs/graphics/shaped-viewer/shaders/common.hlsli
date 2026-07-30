#pragma once

#include "camera.hlsli"

// Shared bindings and the ray payload for the viewer's ray-tracing shaders.
// FrameConstants mirrors sv::frame_constants_gpu (frame_constants.hh) lane-for-lane — keep them in lockstep.

struct FrameConstants
{
    Camera camera;

    uint mesh_is_indexed; // 0 => the bound mesh is a plain triangle list; see mesh.hlsli
};

ConstantBuffer<FrameConstants> frame : register(b0);

// One primary ray's result: the shaded (or sky) color for its pixel.
// slib compiles ray-tracing shaders at SM 6.8, which requires the DXR 1.1 payload annotation: the
// [raypayload] attribute plus per-field read/write stage qualifiers. The closest-hit and miss write the
// color; the raygen (caller) reads it back.
struct [raypayload] Payload
{
    float4 color : read(caller) : write(closesthit, miss);
};
