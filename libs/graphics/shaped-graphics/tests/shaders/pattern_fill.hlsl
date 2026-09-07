// Shader fixture for the render-routine end-to-end test (routine-test.cc). Fills a buffer with a
// generated pattern (gValues[i] = i*3 + 7) so a test can dispatch it and verify every element.
#pragma sc group 0
namespace pattern_fill_bindings
{
    RWStructuredBuffer<uint> gValues;
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    pattern_fill_bindings::gValues[tid.x] = tid.x * 3u + 7u;
}
