// svgf_atrous.hlsl and the part of svgf_common.hlsli it uses, written by hand in MSL, for the benchmark against SGL's
// text on metal.
// The algorithm is the HLSL's line for line; the bindings are SGL's svgf_atrous_images and svgf_atrous_constants,
// slot for slot, so one binding group serves both contenders.

#include <metal_stdlib>
using namespace metal;

constant uint k_svgf_has_albedo = 1u << 0;
constant uint k_svgf_remodulate_out = 1u << 2;
constant uint k_svgf_has_specular_albedo = 1u << 3;
constant float k_svgf_albedo_floor = 1e-3;

// sg writes each slot's byte length into sgl_lengths, which this shader declares only to keep SGL's layout.
struct svgf_atrous_images_arguments
{
    texture2d<float> svgf_atrous_images_source [[id(0)]];
    texture2d<float> svgf_atrous_images_normal_depth [[id(1)]];
    texture2d<float> svgf_atrous_images_albedo [[id(2)]];
    texture2d<float> svgf_atrous_images_specular_albedo [[id(3)]];
    texture2d<float> svgf_atrous_images_color [[id(4)]];
    texture2d<float, access::write> svgf_atrous_images_target [[id(5)]];
    uint sgl_lengths [[id(6)]][6];
};

struct svgf_atrous_constants_data
{
    int step_;
    uint flags;
    float luminance_sigma;
    float normal_power;
    float depth_sigma;
};

float3 svgf_reflectance(float3 albedo, float3 specular_albedo, uint flags)
{
    if ((flags & k_svgf_has_specular_albedo) != 0)
        albedo += specular_albedo;
    return max(albedo, k_svgf_albedo_floor);
}

float svgf_luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

bool svgf_is_surface(float3 n, float depth)
{
    return dot(n, n) > 0 && depth > 0;
}

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

#pragma sc numthreads 8 8 1
kernel void atrous_cs(uint3 id [[thread_position_in_grid]],
                      constant svgf_atrous_images_arguments& svgf_atrous_images_group [[buffer(0)]],
                      constant svgf_atrous_constants_data& svgf_atrous_constants [[buffer(4)]])
{
    constant auto& g = svgf_atrous_images_group;
    constant auto& k = svgf_atrous_constants;

    uint2 size = uint2(g.svgf_atrous_images_target.get_width(), g.svgf_atrous_images_target.get_height());
    if (any(id.xy >= size))
        return;

    int2 p = int2(id.xy);
    uint flags = k.flags;

    float4 center = g.svgf_atrous_images_source.read(uint2(p), 0);
    float4 nd_p = g.svgf_atrous_images_normal_depth.read(uint2(p), 0);
    float center_lum = svgf_luminance(center.rgb);

    float blurred_variance = 0;
    float blur_weight = 0;
    for (int by = -1; by <= 1; ++by)
    {
        for (int bx = -1; bx <= 1; ++bx)
        {
            int2 q = clamp(p + int2(bx, by), int2(0, 0), int2(size) - 1);
            float const w = (bx == 0 ? 0.5 : 0.25) * (by == 0 ? 0.5 : 0.25);
            blurred_variance += g.svgf_atrous_images_source.read(uint2(q), 0).a * w;
            blur_weight += w;
        }
    }
    blurred_variance /= blur_weight;

    float lum_width = k.luminance_sigma * sqrt(max(blurred_variance, 0.0)) + 1e-6;

    const float kernel_[5] = {1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0};

    float3 sum = float3(0, 0, 0);
    float variance_sum = 0;
    float weight_sum = 0;
    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 offset = int2(dx, dy) * k.step_;
            int2 q = p + offset;
            if (any(q < 0) || any(q >= int2(size)))
                continue;

            float4 c = g.svgf_atrous_images_source.read(uint2(q), 0);
            float4 nd_q = g.svgf_atrous_images_normal_depth.read(uint2(q), 0);

            float w = kernel_[dx + 2] * kernel_[dy + 2];
            w *= svgf_geometry_weight(nd_p.xyz, nd_p.w, nd_q.xyz, nd_q.w, float2(offset), k.normal_power,
                                      k.depth_sigma);
            w *= exp(-abs(svgf_luminance(c.rgb) - center_lum) / lum_width);

            sum += c.rgb * w;
            variance_sum += c.a * w * w;
            weight_sum += w;
        }
    }

    float3 result = sum / weight_sum;
    float variance = variance_sum / (weight_sum * weight_sum);

    float out_alpha = variance;
    if ((flags & k_svgf_remodulate_out) != 0)
    {
        if ((flags & k_svgf_has_albedo) != 0)
            result *= svgf_reflectance(g.svgf_atrous_images_albedo.read(uint2(p), 0).rgb,
                                       g.svgf_atrous_images_specular_albedo.read(uint2(p), 0).rgb, flags);
        out_alpha = g.svgf_atrous_images_color.read(uint2(p), 0).a;
    }

    g.svgf_atrous_images_target.write(float4(result, out_alpha), id.xy);
}
