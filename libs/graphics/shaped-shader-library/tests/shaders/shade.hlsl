// Reads the annotated group through the header that declares it, so the rewrite has to reach an include.
#include "frame_bindings.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    float2 uv = (float2(tid.xy) + 0.5f) / 64.0f;
    frame_bindings::histogram[tid.x] = frame_bindings::albedo.SampleLevel(frame_bindings::linear_sampler, uv, 0).x;
}
