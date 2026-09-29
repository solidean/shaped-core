#pragma once

#include "openpbr.hlsli"

// What the denoiser guides say about one surface, as pure functions of it.
//
// Here rather than in pt_shade.hlsli so a probe can call them: that file declares the path tracer's bindings, and a
// compute shader cannot bring a TLAS along to ask what a material's albedo is.
// The guides are the one part of the shading a denoiser reads directly, so they are also the part worth asserting on
// numerically rather than through an image.

/// The reflectance a denoiser divides out before it filters, so texture detail is not averaged away with the noise.
///
/// DIFFUSE reflectance, which is what the vendor denoisers take as their albedo guide; the specular half is theirs to
/// ask for separately.
/// So it is what the base and subsurface lobes reflect: nothing where the surface is metal or transmits.
float3 pt_diffuse_albedo(sv::surface s)
{
    float3 const diffuse = lerp(s.base_color, s.subsurface_color, saturate(s.subsurface_weight));
    return saturate(s.base_weight * diffuse * (1.0 - saturate(s.base_metalness)) * (1.0 - saturate(s.transmission_weight)));
}

/// The specular half of the same guide: what this surface reflects at normal incidence.
///
/// Derived the way `bsdf_prepare` derives `spec_f0` and `metal_f0`, and blended between them by metalness, so the
/// guide describes the lobe the shading actually uses rather than a second opinion about it.
/// Grazing reflectance is Schlick's business and the guide does not carry it: a denoiser wants to know what a
/// reflection is worth, not what it becomes at the horizon.
float3 pt_specular_albedo(sv::surface s)
{
    float const ior = max(1.0 + 1e-3, s.specular_ior);
    float const r0 = (ior - 1.0) / (ior + 1.0);

    float3 const dielectric = saturate(s.specular_weight) * saturate(saturate(s.specular_color) * (r0 * r0));
    float3 const metal = saturate(s.base_weight * s.base_color);
    return saturate(lerp(dielectric, metal, saturate(s.base_metalness)));
}

/// How sharp the sharpest specular lobe on this surface is, which is what sizes a denoiser's reflection filter.
///
/// The coat wins where it is present, because it sits outermost: a rough base under a smooth coat still shows a sharp
/// reflection, and filtering that as if it were the base's would smear the one feature the coat exists to add.
/// Perceptual roughness rather than the GGX alpha, since that is what every denoiser's guide is specified in.
float pt_roughness(sv::surface s)
{
    return saturate(lerp(s.specular_roughness, s.coat_roughness, saturate(s.coat_weight)));
}
