// The guides a path tracer produces, repacked into the encodings NRD reads.
//
// NRD does not take a normal, a roughness and a hit distance as such: it takes one normal-roughness texture in an
// encoding its own build chose, and radiance whose alpha carries a hit distance normalized against a curve that
// depends on view depth and roughness.
// Both are exact formulas rather than conventions, so this pass calls NRD's own header for them — see
// extern/nrd/CMakeLists.txt, which copies it here, and never reimplement one of these.
//
// Everything is at the denoise input extent, one thread per pixel.
// The motion guide is absent on purpose: NRD reads ours untouched, with the sign and the pixels-to-UV scale carried on
// `CommonSettings::motionVectorScale`.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

#include "nrd/NRD.hlsli"

struct nrd_repack_constants
{
    // REBLUR's hit-distance normalization curve, which must be the parameters the denoiser itself was given.
    float hit_distance_a;
    float hit_distance_b;
    float hit_distance_c;

    // What a primary ray that hit nothing writes as view depth.
    // Sky is not a surface NRD can reproject, and it recognizes one by an out-of-range depth rather than by a flag.
    float sky_view_z;

    // The rotation of world-to-view, one row per lane; the translation is left out because only a direction is
    // turned by it.
    float4 world_to_view_row0;
    float4 world_to_view_row1;
    float4 world_to_view_row2;

    // tan(fov / 2) on each axis, which is what turns a pixel into the view ray through it.
    float2 tan_half;
    float2 _pad;
};

#pragma sc push_constants
ConstantBuffer<nrd_repack_constants> gConstants;

#pragma sc group 0
namespace nrd_repack_bindings
{
    Texture2D<float4> gDiffuse;      // diffuse radiance, linear HDR
    Texture2D<float4> gSpecular;     // specular radiance, linear HDR
    Texture2D<float4> gNormal;       // world-space shading normal, in rgb
    Texture2D<float4> gRoughness;    // linear (perceptual) roughness, in r — which is NRD's LINEAR encoding
    Texture2D<float4> gDepth;        // linear view depth, in r; 0 or less where the ray missed
    Texture2D<float2> gHitDistance;  // r the diffuse path's first secondary hit, g the specular path's
    Texture2D<float4> gAlbedo;          // diffuse reflectance at the primary hit
    Texture2D<float4> gSpecularAlbedo;  // normal-incidence specular reflectance (F0)

    RWTexture2D<float4> gNormalRoughness;
    RWTexture2D<float> gViewZ;
    RWTexture2D<float4> gDiffuseRadianceHitDistance;
    RWTexture2D<float4> gSpecularRadianceHitDistance;

    // The two factors this pass divided by, for nrd_resolve.hlsl to multiply back.
    RWTexture2D<float4> gDiffuseFactor;
    RWTexture2D<float4> gSpecularFactor;
}

using namespace nrd_repack_bindings;

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    gViewZ.GetDimensions(size.x, size.y);
    if (any(id.xy >= size))
        return;

    int3 p = int3(int2(id.xy), 0);

    float const depth = gDepth.Load(p).r;
    bool const is_surface = depth > 0;
    float const view_z = is_surface ? depth : gConstants.sky_view_z;

    // A missed ray has no normal to speak of; anything normalized will do, and leaving it zero makes NRD's own
    // normalize produce NaNs that then spread through the history.
    float3 normal = gNormal.Load(p).rgb;
    normal = is_surface ? normalize(normal) : float3(0, 0, 1);

    float const roughness = saturate(gRoughness.Load(p).r);

    gViewZ[id.xy] = view_z;
    gNormalRoughness[id.xy] = NRD_FrontEnd_PackNormalAndRoughness(normal, roughness, 0.0);

    // What NRD asks for and what makes it keep texture: radiance carrying no material information, so a surface's
    // pattern is not filtered as though it were noise.
    // `NRD_MaterialFactors` is NRD's own, for the reason the header gives, and it floors both factors well above
    // zero — so the division below needs no guard of its own.
    //
    // It reads the normal and the view direction only through `abs(dot(N, V))`, so both are handed over in VIEW
    // space, which is the cheaper of the two to reach from here.
    float2 const ndc = float2((float2(id.xy) + 0.5) / float2(size) * float2(2, -2) + float2(-1, 1));
    float3 const view_ray = float3(ndc * gConstants.tan_half, 1.0);
    float3 const V = -normalize(view_ray);
    float3 const n_view = float3(dot(gConstants.world_to_view_row0.xyz, normal),
                                 dot(gConstants.world_to_view_row1.xyz, normal),
                                 dot(gConstants.world_to_view_row2.xyz, normal));

    float3 diffuse_factor;
    float3 specular_factor;
    NRD_MaterialFactors(n_view, V, gAlbedo.Load(p).rgb, gSpecularAlbedo.Load(p).rgb, roughness, diffuse_factor,
                        specular_factor);

    gDiffuseFactor[id.xy] = float4(diffuse_factor, 0);
    gSpecularFactor[id.xy] = float4(specular_factor, 0);

    float3 const hit_distance_params = float3(gConstants.hit_distance_a, gConstants.hit_distance_b, gConstants.hit_distance_c);
    float2 const hit_distance = gHitDistance.Load(p);

    // The diffuse lobe normalizes against roughness 1: its spread does not depend on the surface's roughness, which is
    // what the curve's roughness argument models.
    float const diffuse_norm_hit_distance = REBLUR_FrontEnd_GetNormHitDist(hit_distance.r, view_z, hit_distance_params, 1.0);
    float const specular_norm_hit_distance = REBLUR_FrontEnd_GetNormHitDist(hit_distance.g, view_z, hit_distance_params, roughness);

    gDiffuseRadianceHitDistance[id.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        gDiffuse.Load(p).rgb / diffuse_factor, diffuse_norm_hit_distance, true);
    gSpecularRadianceHitDistance[id.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        gSpecular.Load(p).rgb / specular_factor, specular_norm_hit_distance, true);
}
