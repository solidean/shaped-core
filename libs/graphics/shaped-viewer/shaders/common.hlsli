#pragma once

#include "background.hlsli" // Background + the SH evaluation the miss and the closest-hit use
#include "camera.hlsli"
#include "mesh.hlsli"    // Triangle3 + mesh_triangle
#include "pbr.hlsli"     // PbrMaterial + the shared PBR helpers

// Shared bindings and the ray payload for the viewer's ray-tracing shaders.
// FrameConstants mirrors sv::frame_constants_gpu (frame_constants.hh) lane-for-lane — keep them in lockstep.

struct FrameConstants
{
    Camera camera;

    uint mesh_is_indexed; // 0 => the bound mesh is a plain triangle list; see mesh.hlsli
};

// The whole group DXR's global root signature covers, declared once here rather than a piece per stage.
//
// A ray-tracing pipeline's stages share one root signature, so raygen, miss and closest-hit must agree on every
// address. They used to agree because three files hand-wrote matching registers; now they agree because they
// read one declaration — and slib's binding pass writes the addresses into each stage's flattened source.
//
// See shaped-shader-library/docs/binding-preprocessor.md.
#pragma sc group 0
namespace flat_bindings
{
    RaytracingAccelerationStructure scene;
    RWTexture2D<float4> Output;
    ConstantBuffer<FrameConstants> frame;
    ConstantBuffer<Background> background;
    StructuredBuffer<PbrMaterial> Materials;
    StructuredBuffer<float3> Vertices;
    StructuredBuffer<uint> Indices;
}

// One primary ray's result: the shaded (or sky) color for its pixel.
// slib compiles ray-tracing shaders at SM 6.8, which requires the DXR 1.1 payload annotation: the
// [raypayload] attribute plus per-field read/write stage qualifiers. The closest-hit and miss write the
// color; the raygen (caller) reads it back.
struct [raypayload] Payload
{
    float4 color : read(caller) : write(closesthit, miss);
};
