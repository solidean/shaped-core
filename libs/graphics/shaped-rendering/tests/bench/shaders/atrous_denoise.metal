// atrous_denoise.hlsl written by hand in MSL, for the benchmark against SGL's text on metal.
// The algorithm is the HLSL's line for line; the bindings are SGL's atrous_images and atrous_constants, slot for slot,
// so one binding group serves both contenders.
// MSL has no globals for resources, so the helpers take the argument buffer and the constants as parameters.

#include <metal_stdlib>
using namespace metal;

constant uint k_has_albedo = 1u << 0;
constant uint k_has_normal = 1u << 1;
constant uint k_has_depth = 1u << 2;
constant uint k_demodulate_in = 1u << 3;
constant uint k_remodulate_out = 1u << 4;
constant uint k_has_specular_albedo = 1u << 5;
constant uint k_has_sample_counts = 1u << 6;
constant float k_albedo_floor = 1e-3;

// sg writes each slot's byte length into sgl_lengths, which this shader declares only to keep SGL's layout.
struct atrous_images_arguments
{
    texture2d<float> atrous_images_source [[id(0)]];
    texture2d<float> atrous_images_albedo [[id(1)]];
    texture2d<float> atrous_images_specular_albedo [[id(2)]];
    texture2d<float> atrous_images_normal [[id(3)]];
    texture2d<float> atrous_images_depth [[id(4)]];
    texture2d<float> atrous_images_sample_counts [[id(5)]];
    texture2d<float, access::write> atrous_images_target [[id(6)]];
    uint sgl_lengths [[id(7)]][7];
};

struct atrous_constants_data
{
    int step_;
    uint flags;
    float luminance_scale;
    float normal_power;
    float depth_sigma;
};

float luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float3 albedo_at(constant atrous_images_arguments& g, constant atrous_constants_data& k, int2 p)
{
    float3 a = g.atrous_images_albedo.read(uint2(p), 0).rgb;
    if ((k.flags & k_has_specular_albedo) != 0)
        a += g.atrous_images_specular_albedo.read(uint2(p), 0).rgb;
    return max(a, k_albedo_floor);
}

float4 source_at(constant atrous_images_arguments& g, constant atrous_constants_data& k, int2 p)
{
    float4 c = g.atrous_images_source.read(uint2(p), 0);
    if ((k.flags & k_demodulate_in) != 0)
        c.rgb /= albedo_at(g, k, p);
    return c;
}

#pragma sc numthreads 8 8 1
kernel void main_cs(uint3 id [[thread_position_in_grid]],
                    constant atrous_images_arguments& atrous_images_group [[buffer(0)]],
                    constant atrous_constants_data& atrous_constants [[buffer(4)]])
{
    constant auto& g = atrous_images_group;
    constant auto& k = atrous_constants;

    uint2 size = uint2(g.atrous_images_target.get_width(), g.atrous_images_target.get_height());
    if (any(id.xy >= size))
        return;

    int2 p = int2(id.xy);
    uint flags = k.flags;

    float4 center = source_at(g, k, p);
    float center_lum = luminance(center.rgb);

    float3 center_normal = float3(0, 0, 0);
    bool center_has_normal = false;
    if ((flags & k_has_normal) != 0)
    {
        float3 n = g.atrous_images_normal.read(uint2(p), 0).rgb;
        center_has_normal = dot(n, n) > 0;
        center_normal = center_has_normal ? normalize(n) : n;
    }

    float center_depth = 0;
    if ((flags & k_has_depth) != 0)
        center_depth = g.atrous_images_depth.read(uint2(p), 0).r;

    float luminance_scale = k.luminance_scale;
    if ((flags & k_has_sample_counts) != 0)
        luminance_scale /= sqrt(max(g.atrous_images_sample_counts.read(uint2(p), 0).r, 1.0));
    float lum_width = luminance_scale * (abs(center_lum) + 1e-2) + 1e-6;

    const float kernel_[5] = {1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0};

    float3 sum = float3(0, 0, 0);
    float weight_sum = 0;

    for (int dy = -2; dy <= 2; ++dy)
    {
        for (int dx = -2; dx <= 2; ++dx)
        {
            int2 offset = int2(dx, dy) * k.step_;
            int2 q = p + offset;
            if (any(q < 0) || any(q >= int2(size)))
                continue;

            float4 c = source_at(g, k, q);
            float w = kernel_[dx + 2] * kernel_[dy + 2];

            float lum_ratio = (luminance(c.rgb) - center_lum) / lum_width;
            w *= exp(-lum_ratio * lum_ratio);

            if ((flags & k_has_normal) != 0)
            {
                float3 n = g.atrous_images_normal.read(uint2(q), 0).rgb;
                bool tap_has_normal = dot(n, n) > 0;
                if (center_has_normal != tap_has_normal)
                    w = 0;
                else if (center_has_normal)
                    w *= pow(saturate(dot(center_normal, normalize(n))), k.normal_power);
            }

            if ((flags & k_has_depth) != 0)
            {
                float d = g.atrous_images_depth.read(uint2(q), 0).r;
                bool center_missed = center_depth <= 0;
                bool tap_missed = d <= 0;
                if (center_missed != tap_missed)
                    w = 0;
                else if (!center_missed)
                {
                    float allowed = k.depth_sigma * center_depth * length(float2(offset)) + 1e-5;
                    w *= exp(-abs(d - center_depth) / allowed);
                }
            }

            sum += c.rgb * w;
            weight_sum += w;
        }
    }

    float3 result = sum / weight_sum;

    if ((flags & k_remodulate_out) != 0)
        result *= albedo_at(g, k, p);

    g.atrous_images_target.write(float4(result, center.a), id.xy);
}
