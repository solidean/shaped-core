// 280 values live at once, more than the 256 VGPRs a lane has, so ACO spills, to LDS first.
// N is tuned: at 275 or 285 the scheduler happens not to hoist a reload above its own spill.
[[vk::binding(0, 0)]] StructuredBuffer<uint> input;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> output;

#define N 280

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint t = id.x;
    uint v[N];
    [unroll] for (uint i = 0; i < N; ++i)
        v[i] = input[i];
    for (uint m = 0; m < 2; ++m)
    {
        [unroll] for (uint i = 0; i < N; ++i)
            v[i] = v[i] * 3 + (t ^ v[(i * 7) % N]);
    }
    uint acc = t;
    [unroll] for (uint i = 0; i < N; ++i)
        acc ^= v[i];
    output[t] = acc;
}
