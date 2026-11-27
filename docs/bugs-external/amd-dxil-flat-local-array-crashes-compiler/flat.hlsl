// Eight 16-float elements held as one flat `float[128]`, which AMD's compiler crashes on.
// Each step below is needed: -DNO_FILL, -DNO_REWRITE, -DNO_FIELD, -DUNCONDITIONAL and -DREWRITTEN=7 each compile.
// So do -DELEMENTS=4 and -DELEMENTS=16, which change the array's length.
StructuredBuffer<uint> params : register(t0);
RWStructuredBuffer<float4> target : register(u1);
#ifndef ELEMENTS
#define ELEMENTS 8
#endif
#ifndef REWRITTEN
#define REWRITTEN ELEMENTS
#endif

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float q[ELEMENTS * 16];
    // 1. zero-filled where it is declared
#ifndef NO_FILL
    [unroll] for (int k = 0; k < ELEMENTS * 16; ++k)
        q[k] = 0.0;
#endif
    // 2. rewritten whole, every element and field, under a branch
#ifndef NO_REWRITE
#ifndef UNCONDITIONAL
    if (params[0] == 0u)
#endif
    {
        [loop] for (int z = 0; z < REWRITTEN; ++z)
            [unroll] for (int f = 0; f < 16; ++f)
                q[z * 16 + f] = 0.0;
    }
#endif
    // 3. one field of each element written in a loop of a run-time count
    uint n = params[1];
#ifndef NO_FIELD
    [loop] for (int g = 0; g < int(n); ++g)
        q[g * 16 + 15] = 0.5;
#endif
    [loop] for (int i = 0; i < int(n); ++i)
        [unroll] for (int r = 0; r < 4; ++r)
            target[id.x * 32 + i * 4 + r]
                = float4(q[i * 16 + r * 4], q[i * 16 + r * 4 + 1], q[i * 16 + r * 4 + 2], q[i * 16 + r * 4 + 3]);
}
