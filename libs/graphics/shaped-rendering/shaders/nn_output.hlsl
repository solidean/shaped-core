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
    /// The TENSOR's extent, which is what indexes the source.
    uint width;
    uint height;

    /// The IMAGE's extent. The padding beyond it was only ever there to give the pools a size they could halve, so
    /// nothing is written for it.
    uint target_width;
    uint target_height;

    /// This tile's INTERIOR, which is the only part of the tensor worth keeping.
    /// The overlap around it exists so the interior sees the same neighbourhood a whole-image run would, and it is
    /// discarded rather than written.
    uint write_width;
    uint write_height;

    /// Where the interior lands in the image, and where it starts inside the tensor.
    int target_offset_x;
    int target_offset_y;
    int read_offset_x;
    int read_offset_y;

    float input_scale; // the same one nn_input.hlsl applied; this divides by it
    float _pad0;
    float _pad1;
    float _pad2;
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
    if (id.x >= gConstants.write_width || id.y >= gConstants.write_height)
        return;

    int2 const dst = int2(gConstants.target_offset_x + int(id.x), gConstants.target_offset_y + int(id.y));
    if (dst.x >= int(gConstants.target_width) || dst.y >= int(gConstants.target_height))
        return;

    uint2 const src = uint2(uint(gConstants.read_offset_x + int(id.x)), uint(gConstants.read_offset_y + int(id.y)));
    uint const base = (src.y * gConstants.width + src.x) * 3u;
    float3 value = float3(gSource[base + 0], gSource[base + 1], gSource[base + 2]);

    value = max(sanitize(value), float3(0, 0, 0));
    value = pu_inverse(value);

    // Undoing the scale the input applied, so the result is in the caller's units.
    float const scale = gConstants.input_scale != 0.0 ? 1.0 / gConstants.input_scale : 0.0;
    gTarget[uint2(dst)] = float4(value * scale, 1);
}
