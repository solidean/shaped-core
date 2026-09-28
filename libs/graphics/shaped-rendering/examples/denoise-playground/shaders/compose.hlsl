// The split view: the tracer's raw image on the left of the divider, the denoiser's output on the right.
//
// Side by side rather than a toggle, because the whole question a denoiser answers is "compared to what". A viewer
// that flips between two images asks its reader to remember one of them; this one does not.
//
// It also tonemaps, which is where the HDR radiance the tracer produces becomes something a bgra8 back buffer can
// show. Without it the linear values clip and the image reads as washed out rather than bright.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

struct compose_constants
{
    float split;    // where the divider sits, in [0, 1] of the width
    float exposure; // multiplies the radiance before the tonemap
    int show_split; // 0 shows the right-hand image everywhere, for a reader who wants one picture
    int _pad;
};

#pragma sc push_constants
ConstantBuffer<compose_constants> gConstants;

#pragma sc group 0
namespace compose_bindings
{
    Texture2D<float4> gRaw;      // the tracer's own output
    Texture2D<float4> gDenoised; // what the denoiser made of it
    RWTexture2D<float4> gTarget;
}

using namespace compose_bindings;

// ACES, the Narkowicz fit: cheap, and it rolls the highlights off instead of clipping them.
float3 tonemap(float3 c)
{
    c = max(c, 0.0);
    float3 n = c * (2.51 * c + 0.03);
    float3 d = c * (2.43 * c + 0.59) + 0.14;
    return saturate(n / d);
}

// The back buffer is unorm rather than _srgb, so the encode happens here.
// Branchless rather than a ternary on a bool3, which DXC will not select component-wise.
float3 to_srgb(float3 c)
{
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 1e-8), 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(c, 0.0031308));
}

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    gTarget.GetDimensions(size.x, size.y);
    if (any(id.xy >= size))
        return;

    int2 p = int2(id.xy);
    float split_x = gConstants.split * float(size.x);
    bool left = gConstants.show_split != 0 && float(p.x) < split_x;

    float3 c = left ? gRaw.Load(int3(p, 0)).rgb : gDenoised.Load(int3(p, 0)).rgb;
    float3 out_color = to_srgb(tonemap(c * gConstants.exposure));

    // A one-pixel divider, so the seam is a deliberate line rather than something to look for.
    if (gConstants.show_split != 0 && abs(float(p.x) - split_x) < 1.0)
        out_color = float3(1, 1, 1);

    gTarget[id.xy] = float4(out_color, 1);
}
