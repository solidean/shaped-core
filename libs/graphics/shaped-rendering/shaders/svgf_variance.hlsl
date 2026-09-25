// SVGF's variance pass: how noisy each pixel of the integrated history still is, which is what steers the filter.
//
// A pixel with enough history reads its variance straight off its accumulated moments.
// A young one (a disocclusion, or the first frames) has too few samples for that to mean anything, so it estimates the
// variance spatially instead, from the neighbours that show the same surface — and boosts it, so the filter leans hard
// on exactly the pixels that have the least to go on.
//
// Writes the colour through unchanged, with the variance in alpha, as the first à-trous pass's input.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

#include "svgf_common.hlsli"

struct svgf_variance_constants
{
    float normal_power;
    float depth_sigma;
    float spatial_below; // a history shorter than this estimates its variance spatially
    float _pad;
};

#pragma sc push_constants
ConstantBuffer<svgf_variance_constants> gConstants;

#pragma sc group 0
namespace svgf_variance_bindings
{
    Texture2D<float4> gHistory; // rgb demodulated colour, a history length
    Texture2D<float2> gMoments; // r mean luminance, g mean squared luminance
    Texture2D<float4> gNormalDepth;
    RWTexture2D<float4> gTarget; // rgb colour, a variance
}

using namespace svgf_variance_bindings;

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    gTarget.GetDimensions(size.x, size.y);
    if (any(id.xy >= size))
        return;

    int2 p = int2(id.xy);
    float4 history = gHistory.Load(int3(p, 0));
    float history_length = history.a;

    if (history_length >= gConstants.spatial_below)
    {
        float2 m = gMoments.Load(int3(p, 0));
        gTarget[id.xy] = float4(history.rgb, max(0, m.y - m.x * m.x));
        return;
    }

    // A 7x7 neighbourhood of the same surface, weighted like the filter weights it will later be averaged with.
    float4 nd_p = gNormalDepth.Load(int3(p, 0));
    float2 sum_m = float2(0, 0);
    float weight_sum = 0;
    for (int dy = -3; dy <= 3; ++dy)
    {
        for (int dx = -3; dx <= 3; ++dx)
        {
            int2 q = p + int2(dx, dy);
            if (any(q < 0) || any(q >= int2(size)))
                continue;
            float4 nd_q = gNormalDepth.Load(int3(q, 0));
            float w = svgf_geometry_weight(nd_p.xyz, nd_p.w, nd_q.xyz, nd_q.w, float2(dx, dy), gConstants.normal_power,
                                           gConstants.depth_sigma);
            sum_m += gMoments.Load(int3(q, 0)) * w;
            weight_sum += w;
        }
    }

    // The centre always matches itself, so weight_sum is never zero.
    float2 m = sum_m / weight_sum;
    float variance = max(0, m.y - m.x * m.x);

    // Few samples mean the estimate itself is unreliable, and erring toward more noise filters harder where it matters.
    variance *= gConstants.spatial_below / max(history_length, 1);
    gTarget[id.xy] = float4(history.rgb, variance);
}
