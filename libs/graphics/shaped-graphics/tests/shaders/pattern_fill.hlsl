// Shader fixture for the render-routine end-to-end test (routine-test.cc). Fills a buffer with a
// generated pattern (gValues[i] = i*3 + 7) so a test can dispatch it and verify every element.
RWStructuredBuffer<uint> gValues : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    gValues[tid.x] = tid.x * 3u + 7u;
}
