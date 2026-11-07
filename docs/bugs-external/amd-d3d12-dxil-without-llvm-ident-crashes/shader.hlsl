// Any shader does: the crash is in reading the module, before anything it computes matters.
StructuredBuffer<int> source : register(t0);
RWStructuredBuffer<int> target : register(u1);

[numthreads(64, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID)
{
    target[id.x] = source[id.x] + 1;
}
