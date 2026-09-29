// The transfer function the denoise network was trained through.
//
// A path tracer's radiance spans many orders of magnitude and a network cannot see that directly, so OIDN maps it
// through a PERCEPTUALLY UNIFORM curve first and inverts the curve afterwards.
// Every constant below is OIDN's, because the weights were trained against exactly this curve — a different one that
// merely looks similar is a systematically wrong image rather than a slightly different one.
//
// Three pieces: linear near zero, a power in the middle, a logarithm above.
// See `core/color.h` in the OIDN sources, from which these are transcribed.

#ifndef SR_NN_TRANSFER_HLSLI
#define SR_NN_TRANSFER_HLSLI

// The largest value the curve is normalized against, which is also the largest a half float holds.
static const float k_pu_y_max = 65504.0;

static const float k_pu_a = 1.41283765e+03;
static const float k_pu_b = 1.64593172e+00;
static const float k_pu_c = 4.31384981e-01;
static const float k_pu_d = -2.94139609e-03;
static const float k_pu_e = 1.92653254e-01;
static const float k_pu_f = 6.26026094e-03;
static const float k_pu_g = 9.98620152e-01;
static const float k_pu_y0 = 1.57945760e-06;
static const float k_pu_y1 = 3.22087631e-02;
static const float k_pu_x0 = 2.23151711e-03;
static const float k_pu_x1 = 3.70974749e-01;

float pu_forward_raw(float y)
{
    if (y <= k_pu_y0)
        return k_pu_a * y;
    if (y <= k_pu_y1)
        return k_pu_b * pow(y, k_pu_c) + k_pu_d;
    return k_pu_e * log(y + k_pu_f) + k_pu_g;
}

float pu_inverse_raw(float x)
{
    if (x <= k_pu_x0)
        return x / k_pu_a;
    if (x <= k_pu_x1)
        return pow((x - k_pu_d) / k_pu_b, 1.0 / k_pu_c);
    return exp((x - k_pu_g) / k_pu_e) - k_pu_f;
}

// What the curve is divided by so that the largest representable value maps to 1.
//
// Derived rather than written down, because the alternative is a literal that can drift from the constants above
// without anything noticing — OIDN computes it the same way, in its transfer function's constructor.
float pu_norm_scale()
{
    return 1.0 / pu_forward_raw(k_pu_y_max);
}

float3 pu_forward(float3 y)
{
    return float3(pu_forward_raw(y.x), pu_forward_raw(y.y), pu_forward_raw(y.z)) * pu_norm_scale();
}

float3 pu_inverse(float3 x)
{
    float3 const s = x / pu_norm_scale();
    return float3(pu_inverse_raw(s.x), pu_inverse_raw(s.y), pu_inverse_raw(s.z));
}

// NaN and infinity reach the network as whatever the hardware does with them, and one of either poisons every pixel
// its receptive field touches — so both are removed on the way in.
float3 sanitize(float3 v)
{
    // `or` rather than `||`, which HLSL 2021 requires on a vector — the short-circuiting form is scalar only.
    return select(or(isnan(v), isinf(v)), float3(0, 0, 0), v);
}

#endif
