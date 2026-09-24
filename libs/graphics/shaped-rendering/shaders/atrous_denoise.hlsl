// One pass of edge-avoiding à-trous wavelet denoising (Dammertz et al. 2010).
// Paired with sr::atrous_denoise_routine, which dispatches this once per pass with the tap spacing doubling each time.
//
// Every pass is a 5x5 B3-spline kernel whose taps sit `step` pixels apart, each weighted down by how much its normal,
// depth and luminance disagree with the centre's.
// A tap across an edge in any of the three counts for nothing, which keeps edges sharp while flat regions average wide.
//
// The luminance edge-stop is relative to the noise the input is expected to carry.
// `luminance_scale` is the caller's sigma times 1/sqrt(sample count), so a one-sample frame is smoothed hard and a
// deep mean is averaged only across differences a few of its own, much smaller, noise widths wide.
//
// Guides are optional.
// A missing one is bound to the colour texture as a stand-in with its flag clear, so every slot of the group is filled
// and the shader never reads what the flag says is not there.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

static const uint k_has_albedo = 1u << 0;
static const uint k_has_normal = 1u << 1;
static const uint k_has_depth = 1u << 2;
static const uint k_demodulate_in = 1u << 3; // this pass reads raw colour and divides the albedo out
static const uint k_remodulate_out = 1u << 4; // this pass multiplies the albedo back in before writing

// A zero albedo would make demodulation divide by zero, and clamping both directions by the same floor keeps the
// round trip exact.
static const float k_albedo_floor = 1e-3;

struct atrous_constants
{
    int step;              // tap spacing in pixels: 1, 2, 4, ...
    uint flags;            // the k_* bits above
    float luminance_scale; // the luminance edge-stop's width, relative to the pixel's own luminance
    float normal_power;    // exponent on dot(n_p, n_q)
    float depth_sigma;     // allowed depth difference, as a fraction of the centre's depth per pixel of tap distance
    float3 _pad;
};

#pragma sc push_constants
ConstantBuffer<atrous_constants> gConstants;

#pragma sc group 0
namespace atrous_bindings
{
    Texture2D<float4> gSource;
    Texture2D<float4> gAlbedo;
    Texture2D<float4> gNormal;
    Texture2D<float4> gDepth;
    RWTexture2D<float4> gTarget;
}

using namespace atrous_bindings;

float luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float3 albedo_at(int2 p)
{
    return max(gAlbedo.Load(int3(p, 0)).rgb, k_albedo_floor);
}

// The source's colour at `p`, in the space this pass filters in: divided by the albedo on the first pass when asked.
float4 source_at(int2 p)
{
    float4 c = gSource.Load(int3(p, 0));
    if ((gConstants.flags & k_demodulate_in) != 0)
        c.rgb /= albedo_at(p);
    return c;
}

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    gTarget.GetDimensions(size.x, size.y);
    if (any(id.xy >= size))
        return;

    int2 p = int2(id.xy);
    uint flags = gConstants.flags;

    float4 centre = source_at(p);
    float centre_lum = luminance(centre.rgb);

    // A zero normal is a pixel with no surface (a missed ray): it averages only with other such pixels, as depth does.
    float3 centre_normal = float3(0, 0, 0);
    bool centre_has_normal = false;
    if ((flags & k_has_normal) != 0)
    {
        float3 n = gNormal.Load(int3(p, 0)).rgb;
        centre_has_normal = dot(n, n) > 0;
        centre_normal = centre_has_normal ? normalize(n) : n;
    }

    float centre_depth = 0;
    if ((flags & k_has_depth) != 0)
        centre_depth = gDepth.Load(int3(p, 0)).r;

    // Relative, so the edge-stop means the same thing for a dim corner and a bright highlight.
    // The additive floor keeps a black pixel from refusing every neighbour.
    float lum_width = gConstants.luminance_scale * (abs(centre_lum) + 1e-2) + 1e-6;

    static const float kernel[5] = {1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0};

    float3 sum = float3(0, 0, 0);
    float weight_sum = 0;

    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 offset = int2(dx, dy) * gConstants.step;
            int2 q = p + offset;
            if (any(q < 0) || any(q >= int2(size)))
                continue;

            float4 c = source_at(q);
            float w = kernel[dx + 2] * kernel[dy + 2];

            // Gaussian rather than exponential in the difference: a tap several noise widths away must count for
            // nothing, or a converged mean's detail is averaged away by the long tail of many such taps.
            float lum_ratio = (luminance(c.rgb) - centre_lum) / lum_width;
            w *= exp(-lum_ratio * lum_ratio);

            if ((flags & k_has_normal) != 0)
            {
                float3 n = gNormal.Load(int3(q, 0)).rgb;
                bool tap_has_normal = dot(n, n) > 0;
                if (centre_has_normal != tap_has_normal)
                    w = 0;
                else if (centre_has_normal)
                    w *= pow(saturate(dot(centre_normal, normalize(n))), gConstants.normal_power);
            }

            if ((flags & k_has_depth) != 0)
            {
                float d = gDepth.Load(int3(q, 0)).r;
                bool centre_missed = centre_depth <= 0;
                bool tap_missed = d <= 0;
                if (centre_missed != tap_missed)
                    w = 0; // the sky and a surface never average into each other
                else if (!centre_missed)
                {
                    float allowed = gConstants.depth_sigma * centre_depth * length(float2(offset)) + 1e-5;
                    w *= exp(-abs(d - centre_depth) / allowed);
                }
            }

            sum += c.rgb * w;
            weight_sum += w;
        }
    }

    // The centre always passes its own edge-stops, so it contributes at least kernel[2]^2 and weight_sum is never zero.
    float3 result = sum / weight_sum;

    if ((flags & k_remodulate_out) != 0)
        result *= albedo_at(p);

    gTarget[id.xy] = float4(result, centre.a);
}
