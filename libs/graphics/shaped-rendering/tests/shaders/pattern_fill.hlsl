// Shader fixture for shaped-rendering-test's pattern_fill_routine. Fills a buffer with a generated
// pattern (gValues[i] = i*3 + 7) so a test can dispatch it and verify every element round-tripped.
RWStructuredBuffer<uint> gValues : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    gValues[tid.x] = tid.x * 3u + 7u;
}
