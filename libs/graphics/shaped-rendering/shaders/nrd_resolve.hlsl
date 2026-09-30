// NRD's two denoised signals, decoded and summed back into one image.
//
// What REBLUR writes is not radiance: it is YCoCg with a normalized hit distance in alpha, and it is de-modulated —
// so a pass that simply added the two textures would produce a plausible-looking wrong picture.
// The decode is NRD's own, for the reason nrd_repack.hlsl gives.
//
// Sky is the exception, and it is why this pass reads the tracer's own radiance as well as NRD's output.
// A pixel past `CommonSettings::denoisingRange` is one NRD holds no surface for, so it leaves the output UNWRITTEN
// there; decoding that would be decoding whatever the scratch last held.
// The test is the same `depth > 0` one nrd_repack.hlsl makes, so the two cannot disagree about which pixels are sky.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.

#include "nrd/NRD.hlsli"

#pragma sc group 0
namespace nrd_resolve_bindings
{
    Texture2D<float4> gDiffuseRadianceHitDistance;  // OUT_DIFF_RADIANCE_HITDIST
    Texture2D<float4> gSpecularRadianceHitDistance; // OUT_SPEC_RADIANCE_HITDIST

    // What nrd_repack.hlsl divided the two signals by, read back rather than recomputed.
    // NRD requires the de-modulation and the modulation to use the same factors, and storing them is what makes that
    // structural instead of two shaders independently agreeing on a camera, a normal and a roughness.
    Texture2D<float4> gDiffuseFactor;
    Texture2D<float4> gSpecularFactor;

    // The tracer's own two halves, and the depth that says where they are all there is.
    Texture2D<float4> gDiffuse;  // diffuse radiance, as the repack received it
    Texture2D<float4> gSpecular; // specular radiance, as the repack received it
    Texture2D<float4> gDepth;    // linear view depth, in r; 0 or less where the primary ray missed

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

    // Sky: NRD wrote nothing here, so the tracer's radiance is the answer and it needs no decode or modulation.
    if (gDepth.Load(p).r <= 0)
    {
        gOutput[id.xy] = float4(gDiffuse.Load(p).rgb + gSpecular.Load(p).rgb, 1);
        return;
    }

    float3 const diffuse
        = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(gDiffuseRadianceHitDistance.Load(p)).rgb * gDiffuseFactor.Load(p).rgb;
    float3 const specular
        = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(gSpecularRadianceHitDistance.Load(p)).rgb * gSpecularFactor.Load(p).rgb;

    gOutput[id.xy] = float4(diffuse + specular, 1);
}
