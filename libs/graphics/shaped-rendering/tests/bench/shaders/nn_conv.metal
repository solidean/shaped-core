// nn_conv.hlsl written by hand in MSL, for the benchmark against SGL's text on metal.
// The algorithm and its blocking are the HLSL's, NN_CONV_TEXELS included; the bindings are SGL's nn_conv_buffers and
// nn_conv_constants, slot for slot, so one binding group serves both contenders.
// Each `#pragma unroll` stands where the HLSL has `[unroll]`, and nowhere else.

#include <metal_stdlib>
using namespace metal;

#define NN_CONV_TEXELS 8

// sg writes each slot's byte length into sgl_lengths, which this shader declares only to keep SGL's layout.
struct nn_conv_buffers_arguments
{
    const device float4* nn_conv_buffers_source_a [[id(0)]];
    const device float4* nn_conv_buffers_source_b [[id(1)]];
    const device float* nn_conv_buffers_weights [[id(2)]];
    device float* nn_conv_buffers_target [[id(3)]];
    uint sgl_lengths [[id(4)]][4];
};

struct nn_conv_constants_data
{
    uint width;
    uint height;
    uint in_channels;
    uint out_channels;
    uint weight_offset;
    uint bias_offset;
    uint in_channels_a;
};

#pragma sc numthreads 64 1 1
kernel void main_cs(uint3 id [[thread_position_in_grid]],
                    constant nn_conv_buffers_arguments& nn_conv_buffers_group [[buffer(0)]],
                    constant nn_conv_constants_data& nn_conv_constants [[buffer(4)]])
{
    const device float4* gSourceA = nn_conv_buffers_group.nn_conv_buffers_source_a;
    const device float4* gSourceB = nn_conv_buffers_group.nn_conv_buffers_source_b;
    const device float* gWeights = nn_conv_buffers_group.nn_conv_buffers_weights;
    device float* gTarget = nn_conv_buffers_group.nn_conv_buffers_target;
    constant auto& gConstants = nn_conv_constants;

    uint const o = id.x;
    uint const x0 = id.y * NN_CONV_TEXELS;
    uint const y = id.z;
    if (o >= gConstants.out_channels || x0 >= gConstants.width || y >= gConstants.height)
        return;

    float const bias = gWeights[gConstants.bias_offset + o];

    float sums[NN_CONV_TEXELS] = {};
#pragma unroll
    for (uint s = 0; s < NN_CONV_TEXELS; ++s)
        sums[s] = bias;

    uint const in_channels = gConstants.in_channels;
    uint const out_channels = gConstants.out_channels;

    uint const column = in_channels * out_channels;

    for (int ky = -1; ky <= 1; ++ky)
    {
        int const sy = int(y) + ky;
        if (sy < 0 || sy >= int(gConstants.height))
            continue;

        uint const row = gConstants.weight_offset + uint(ky + 1) * 3u * column;

        uint texels[NN_CONV_TEXELS + 2] = {};
        bool inside[NN_CONV_TEXELS + 2] = {};
#pragma unroll
        for (uint j = 0; j < NN_CONV_TEXELS + 2; ++j)
        {
            int const sx = int(x0) + int(j) - 1;
            inside[j] = sx >= 0 && sx < int(gConstants.width);
            texels[j] = inside[j] ? uint(sy) * gConstants.width + uint(sx) : 0u;
        }

        uint const a = gConstants.in_channels_a;
        uint const b = in_channels - a;

        for (uint i = 0; i < in_channels; i += 4u)
        {
            bool const from_a = i < a;
            uint const stride4 = (from_a ? a : b) >> 2;
            uint const c4 = (from_a ? i : i - a) >> 2;

            float4 v[NN_CONV_TEXELS + 2] = {};
#pragma unroll
            for (uint j = 0; j < NN_CONV_TEXELS + 2; ++j)
            {
                uint const at = texels[j] * stride4 + c4;
                v[j] = inside[j] ? (from_a ? gSourceA[at] : gSourceB[at]) : float4(0, 0, 0, 0);
            }

#pragma unroll
            for (uint d = 0; d < 4u; ++d)
            {
                uint const at_w = row + (i + d) * out_channels + o;
                float const w0 = gWeights[at_w];
                float const w1 = gWeights[at_w + column];
                float const w2 = gWeights[at_w + 2u * column];

#pragma unroll
                for (uint t = 0; t < NN_CONV_TEXELS; ++t)
                    sums[t] += w0 * v[t][d] + w1 * v[t + 1][d] + w2 * v[t + 2][d];
            }
        }
    }

    uint const base = (y * gConstants.width + x0) * out_channels + o;
#pragma unroll
    for (uint t = 0; t < NN_CONV_TEXELS; ++t)
        if (x0 + t < gConstants.width)
            gTarget[base + t * out_channels] = max(sums[t], 0.0);
}
