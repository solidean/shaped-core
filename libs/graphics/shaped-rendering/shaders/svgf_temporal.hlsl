// SVGF's temporal pass: this frame's fresh samples folded into the history reprojected from the last frame.
//
// Each pixel follows its motion vector back to where its surface was, and takes the history there only if the surface
// matches — same side of a silhouette, similar normal and depth.
// A pixel whose surface was not visible last frame (a disocclusion) starts over from this frame's samples alone, with a
// history length of one; the variance pass then leans on its neighbours until it has more.
//
// Colour is filtered with the albedo divided out, so what accumulates is lighting and texture survives exactly.
// The history carries its own length in alpha, which is what makes a young pixel blend fast and an old one slowly.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

#include "svgf_common.hlsli"

struct svgf_temporal_constants
{
    uint flags;
    float color_alpha_min;   // the smallest weight a new frame gets: 1 / length, but never below this
    float moments_alpha_min; // the same for the luminance moments, which want to track change a little faster
    float max_history;       // where the history length stops growing
    float normal_similarity; // the least dot(n_prev, n) that still counts as the same surface
    float depth_similarity;  // the largest relative depth change that still counts as the same surface
    float2 _pad;
};

#pragma sc push_constants
ConstantBuffer<svgf_temporal_constants> gConstants;

#pragma sc group 0
namespace svgf_temporal_bindings
{
    Texture2D<float4> gColor;  // this frame's noisy radiance; its alpha is the caller's and rides through untouched
    Texture2D<float4> gAlbedo; // diffuse albedo, or a stand-in when k_svgf_has_albedo is clear
    Texture2D<float4> gNormal;
    Texture2D<float4> gDepth;
    Texture2D<float4> gMotion; // this frame's pixel minus last frame's, in pixels

    Texture2D<float4> gPreviousHistory;      // rgb demodulated colour, a history length
    Texture2D<float2> gPreviousMoments;      // r mean luminance, g mean squared luminance
    Texture2D<float4> gPreviousNormalDepth;  // xyz normal, w depth, as last frame saw them

    RWTexture2D<float4> gHistory;
    RWTexture2D<float2> gMoments;
    RWTexture2D<float4> gNormalDepth;
}

using namespace svgf_temporal_bindings;

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    gHistory.GetDimensions(size.x, size.y);
    if (any(id.xy >= size))
        return;

    int2 p = int2(id.xy);
    uint flags = gConstants.flags;

    float3 color = gColor.Load(int3(p, 0)).rgb;
    if ((flags & k_svgf_has_albedo) != 0)
        color /= max(gAlbedo.Load(int3(p, 0)).rgb, k_svgf_albedo_floor);

    float3 n = gNormal.Load(int3(p, 0)).rgb;
    float d = gDepth.Load(int3(p, 0)).r;
    float lum = svgf_luminance(color);
    float2 moments = float2(lum, lum * lum);

    // Where this pixel's surface was last frame: the pixel's centre moved back by its motion.
    float2 previous_centre = float2(p) + 0.5 - gMotion.Load(int3(p, 0)).rg;
    int2 q = int2(floor(previous_centre));

    bool valid = (flags & k_svgf_reset) == 0 && all(q >= 0) && all(q < int2(size));
    if (valid)
    {
        float4 prev_nd = gPreviousNormalDepth.Load(int3(q, 0));
        bool const surface = svgf_is_surface(n, d);
        bool const prev_surface = svgf_is_surface(prev_nd.xyz, prev_nd.w);
        if (surface != prev_surface)
            valid = false;
        else if (surface)
        {
            float const same_normal = dot(normalize(n), normalize(prev_nd.xyz));
            float const depth_change = abs(prev_nd.w - d) / max(d, 1e-4);
            valid = same_normal >= gConstants.normal_similarity && depth_change <= gConstants.depth_similarity;
        }
    }

    float3 out_color = color;
    float2 out_moments = moments;
    float history_length = 1;
    if (valid)
    {
        float4 prev = gPreviousHistory.Load(int3(q, 0));
        history_length = min(prev.a + 1, gConstants.max_history);

        // 1 / length is the exact running mean while the history is young; the floor turns it into an exponential
        // average once it is old, so a lighting change still shows up rather than being averaged away forever.
        float const color_alpha = max(1.0 / history_length, gConstants.color_alpha_min);
        float const moments_alpha = max(1.0 / history_length, gConstants.moments_alpha_min);
        out_color = lerp(prev.rgb, color, color_alpha);
        out_moments = lerp(gPreviousMoments.Load(int3(q, 0)), moments, moments_alpha);
    }

    gHistory[id.xy] = float4(out_color, history_length);
    gMoments[id.xy] = out_moments;
    gNormalDepth[id.xy] = float4(n, d);
}
