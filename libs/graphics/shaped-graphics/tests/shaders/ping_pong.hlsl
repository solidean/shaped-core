// Shader fixture for the transient benchmark (transient-benchmark.cc): one pass of a simulated post-process, reading one render target and writing the next.
// What it computes does not matter; that every texel is touched does.
#pragma sc group 0
namespace ping_pong_bindings
{
    Texture2D<float4> gSource;
    RWTexture2D<float4> gTarget;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    float4 const c = ping_pong_bindings::gSource.Load(int3(tid.xy, 0));
    ping_pong_bindings::gTarget[tid.xy] = c * 0.5f + float4(0.25f, 0.25f, 0.25f, 0.25f);
}
