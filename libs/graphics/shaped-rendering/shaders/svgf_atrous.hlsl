// SVGF's filter: one à-trous pass whose luminance edge-stop is steered by the variance the earlier passes measured.
// Paired with sr::svgf_denoise_routine, which dispatches this once per pass with the tap spacing doubling each time.
//
// The difference from sr's plain à-trous is where the noise estimate comes from.
// There it is modelled from a sample count; here it is measured per pixel, blurred over a 3x3 so one lucky sample does
// not stop a pixel from being filtered, and carried through every pass.
// A pass leaves each pixel's variance as the variance of the average it produced, so the next pass, at twice the spacing,
// filters less where this one already removed the noise.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

#include "svgf_common.hlsli"

struct svgf_atrous_constants
{
    int step;
    uint flags;
    float luminance_sigma; // how many measured noise widths a luminance difference may span and still average
    float normal_power;
    float depth_sigma;
    float3 _pad;
};

#pragma sc push_constants
ConstantBuffer<svgf_atrous_constants> gConstants;

#pragma sc group 0
namespace svgf_atrous_bindings
{
    Texture2D<float4> gSource; // rgb demodulated colour, a variance
    Texture2D<float4> gNormalDepth;
    Texture2D<float4> gAlbedo; // read only on the last pass, and only when k_svgf_has_albedo is set
    RWTexture2D<float4> gTarget;
}

using namespace svgf_atrous_bindings;

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    gTarget.GetDimensions(size.x, size.y);
    if (any(id.xy >= size))
        return;

    int2 p = int2(id.xy);
    uint flags = gConstants.flags;

    float4 centre = gSource.Load(int3(p, 0));
    float4 nd_p = gNormalDepth.Load(int3(p, 0));
    float centre_lum = svgf_luminance(centre.rgb);

    // The variance, blurred over 3x3 before it steers anything: one pixel that happened to draw similar samples would
    // otherwise refuse all its neighbours and stay as noisy as it came in.
    float blurred_variance = 0;
    float blur_weight = 0;
    for (int by = -1; by <= 1; ++by)
    {
        for (int bx = -1; bx <= 1; ++bx)
        {
            int2 q = clamp(p + int2(bx, by), int2(0, 0), int2(size) - 1);
            float const w = (bx == 0 ? 0.5 : 0.25) * (by == 0 ? 0.5 : 0.25);
            blurred_variance += gSource.Load(int3(q, 0)).a * w;
            blur_weight += w;
        }
    }
    blurred_variance /= blur_weight;

    float lum_width = gConstants.luminance_sigma * sqrt(max(blurred_variance, 0)) + 1e-6;

    static const float kernel[5] = {1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0};

    float3 sum = float3(0, 0, 0);
    float variance_sum = 0;
    float weight_sum = 0;
    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 offset = int2(dx, dy) * gConstants.step;
            int2 q = p + offset;
            if (any(q < 0) || any(q >= int2(size)))
                continue;

            float4 c = gSource.Load(int3(q, 0));
            float4 nd_q = gNormalDepth.Load(int3(q, 0));

            float w = kernel[dx + 2] * kernel[dy + 2];
            w *= svgf_geometry_weight(nd_p.xyz, nd_p.w, nd_q.xyz, nd_q.w, float2(offset), gConstants.normal_power,
                                      gConstants.depth_sigma);
            w *= exp(-abs(svgf_luminance(c.rgb) - centre_lum) / lum_width);

            sum += c.rgb * w;
            variance_sum += c.a * w * w;
            weight_sum += w;
        }
    }

    // The centre matches itself on every test, so weight_sum is at least kernel[2]^2.
    float3 result = sum / weight_sum;
    float variance = variance_sum / (weight_sum * weight_sum);

    if ((flags & k_svgf_remodulate_out) != 0)
    {
        if ((flags & k_svgf_has_albedo) != 0)
            result *= max(gAlbedo.Load(int3(p, 0)).rgb, k_svgf_albedo_floor);
        variance = 1; // the output's alpha is opaque, not a variance
    }

    gTarget[id.xy] = float4(result, variance);
}
