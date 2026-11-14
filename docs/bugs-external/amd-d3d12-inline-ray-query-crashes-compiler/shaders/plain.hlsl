// Any compute shader without a ray query: what an application compiles before its first inline trace.
RWStructuredBuffer<uint> statuses : register(u1);

[numthreads(64, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID)
{
    statuses[id.x] = id.x * 3u;
}
