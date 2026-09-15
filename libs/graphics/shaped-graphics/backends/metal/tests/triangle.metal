// The tier-2 raster fixture, kept as source next to the blob compiled from it.
//
//   xcrun -sdk macosx metal -O2 -o triangle.metallib triangle.metal
//
// A full-target triangle in clip space with a constant colour — no vertex buffer, no bindings, because what this
// fixture is for is the rendering scope and the draw rather than the paths those already cover.

#include <metal_stdlib>
using namespace metal;

struct vs_out
{
    float4 position [[position]];
};

vertex vs_out vertex_main(uint vid [[vertex_id]])
{
    // A triangle large enough to cover the whole target, so every texel of a small render target is written.
    const float2 positions[3] = {float2(-1.0, -3.0), float2(-1.0, 1.0), float2(3.0, 1.0)};

    vs_out out;
    out.position = float4(positions[vid], 0.0, 1.0);
    return out;
}

fragment float4 fragment_main()
{
    return float4(0.25, 0.5, 0.75, 1.0);
}
