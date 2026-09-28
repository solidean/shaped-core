#pragma once

// What the three SVGF passes share: the flag bits, the albedo round trip and the edge-stopping tests.
// Paired with sr::svgf_denoise_routine; see docs/denoising.md for how the passes fit together.

static const uint k_svgf_has_albedo = 1u << 0;
static const uint k_svgf_reset = 1u << 1;         // the temporal pass ignores the history: a first frame, or a cut
static const uint k_svgf_remodulate_out = 1u << 2; // this à-trous pass is the last, and multiplies the albedo back in

// A zero albedo would make demodulation divide by zero; one floor used both ways keeps the round trip exact.
static const float k_svgf_albedo_floor = 1e-3;

float svgf_luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// A zero normal or a non-positive depth is a pixel with no surface: the primary ray escaped.
bool svgf_is_surface(float3 n, float depth)
{
    return dot(n, n) > 0 && depth > 0;
}

// Whether a neighbour at `offset` pixels shows the same surface as the centre, as a weight in [0, 1].
// Two missed pixels match each other and nothing else, so the sky never averages into geometry.
float svgf_geometry_weight(float3 n_p, float d_p, float3 n_q, float d_q, float2 offset, float normal_power, float depth_sigma)
{
    bool const surface_p = svgf_is_surface(n_p, d_p);
    bool const surface_q = svgf_is_surface(n_q, d_q);
    if (surface_p != surface_q)
        return 0;
    if (!surface_p)
        return 1;

    float const w_n = pow(saturate(dot(normalize(n_p), normalize(n_q))), normal_power);
    float const allowed = depth_sigma * d_p * max(length(offset), 1.0) + 1e-5;
    return w_n * exp(-abs(d_q - d_p) / allowed);
}
