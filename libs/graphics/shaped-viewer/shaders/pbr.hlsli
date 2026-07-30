#pragma once

// The metallic-roughness material plus the shared PBR helpers the flat path's IBL uses.
// PbrMaterial mirrors sv::pbr_material_gpu (pbr_material.hh) field-for-field — keep them in lockstep.

static const float SV_PI = 3.14159265358979323846;

struct PbrMaterial
{
    float3 base_color;
    float  metallic;
    float3 emissive;
    float  roughness;
};

// Schlick's Fresnel approximation: reflectance at grazing vs. normal incidence, f0 = the normal-incidence value.
float3 fresnel_schlick(float cos_theta, float3 f0)
{
    return f0 + (float3(1, 1, 1) - f0) * pow(saturate(1.0 - cos_theta), 5.0);
}
