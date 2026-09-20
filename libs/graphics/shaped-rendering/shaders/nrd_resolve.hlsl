// NRD's two denoised signals, decoded and summed back into one image.
//
// What REBLUR writes is not radiance: it is YCoCg with a normalized hit distance in alpha, so a pass that simply added
// the two textures would produce a plausible-looking wrong picture.
// The decode is NRD's own, for the reason nrd_repack.hlsl gives.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

#include "nrd/NRD.hlsli"

#pragma sc group 0
namespace nrd_resolve_bindings
{
    Texture2D<float4> gDiffuseRadianceHitDistance;  // OUT_DIFF_RADIANCE_HITDIST
    Texture2D<float4> gSpecularRadianceHitDistance; // OUT_SPEC_RADIANCE_HITDIST

    RWTexture2D<float4> gOutput;
}

using namespace nrd_resolve_bindings;

[numthreads(8, 8, 1)] void main_cs(uint3 id : SV_DispatchThreadID)
{
    uint2 size;
    gOutput.GetDimensions(size.x, size.y);
    if (any(id.xy >= size))
        return;

    int3 p = int3(int2(id.xy), 0);

    float3 const diffuse = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(gDiffuseRadianceHitDistance.Load(p)).rgb;
    float3 const specular = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(gSpecularRadianceHitDistance.Load(p)).rgb;

    gOutput[id.xy] = float4(diffuse + specular, 1);
}
