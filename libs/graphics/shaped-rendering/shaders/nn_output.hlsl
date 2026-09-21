// The network's three output channels, turned back into radiance.
//
// The inverse of nn_input.hlsl's curve, and the inverse of its scale — so a caller that handed in an `input_scale`
// gets an image in the units it started from.
//
// The network's output is sanitized before the curve rather than after: it is a convolution's sum, so it can carry a
// NaN that the inverse would turn into a NaN of a different magnitude rather than removing.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

#include "nn_transfer.hlsli"

struct nn_output_constants
{
    uint width;
    uint height;
    float input_scale; // the same one nn_input.hlsl applied; this divides by it
    float _pad;
};

#pragma sc push_constants
ConstantBuffer<nn_output_constants> gConstants;

#pragma sc group 0
namespace nn_output_bindings
{
    StructuredBuffer<float> gSource; // HWC, three channels
    RWTexture2D<float4> gTarget;
}

using namespace nn_output_bindings;

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gConstants.width || id.y >= gConstants.height)
        return;

    uint const base = (id.y * gConstants.width + id.x) * 3u;
    float3 value = float3(gSource[base + 0], gSource[base + 1], gSource[base + 2]);

    value = max(sanitize(value), float3(0, 0, 0));
    value = pu_inverse(value);

    // Undoing the scale the input applied, so the result is in the caller's units.
    float const scale = gConstants.input_scale != 0.0 ? 1.0 / gConstants.input_scale : 0.0;
    gTarget[id.xy] = float4(value * scale, 1);
}
