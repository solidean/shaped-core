// Reads the annotated group through the header that declares it, so the rewrite has to reach an include.
// The inline-constants block sits beside it in a space of its own, which is the collision stating a space prevents.
#include "frame_bindings.hlsli"

// Chosen so the constant-buffer rules actually bite: `tint` cannot straddle the first 16-byte row, so it
// lands at 16 rather than at 8, and the generated mirror has to carry that gap as padding.
struct shade_constants
{
    float2 uv_scale;
    float3 tint;
    float exposure;
};

#pragma sc push_constants space=9
ConstantBuffer<shade_constants> gConstants;

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    float2 uv = (float2(tid.xy) + 0.5f) / 64.0f;
    float3 sampled = frame_bindings::albedo.SampleLevel(frame_bindings::linear_sampler, uv * gConstants.uv_scale, 0).xyz;
    frame_bindings::histogram[tid.x] = dot(sampled * gConstants.tint, float3(1, 1, 1)) * gConstants.exposure;
}
