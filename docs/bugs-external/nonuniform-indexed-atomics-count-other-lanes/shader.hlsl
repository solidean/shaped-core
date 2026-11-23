// 64 threads; thread i updates element i % 2 of a two-element array of buffers.
// Slot 0 of each buffer counts the threads of its parity, slot 1 keeps the least of their ids.
// Expected: even = {32, 0}, odd = {32, 1}.
//
// Defines, each one variable of the matrix:
//   NO_ADD, NO_MIN    leave that atomic out
//   USE_RESULT        write each atomic's previous value to `results`, so the shader reads it
//   BRANCH            select the element by a branch on the parity, with literal indices, instead of by index
//   UNIFORM_HINT      index without NonUniformResourceIndex, which is undefined for a divergent index: a control only

RWStructuredBuffer<uint> slots[2] : register(u0);
RWStructuredBuffer<uint> results : register(u2);

#ifdef UNIFORM_HINT
#define AT(i) slots[i]
#else
#define AT(i) slots[NonUniformResourceIndex(i)]
#endif

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint const parity = id.x % 2;
    uint added = 0;
    uint least = 0;
#ifdef BRANCH
    if (parity == 0)
    {
#ifndef NO_ADD
        InterlockedAdd(slots[0][0], 1u, added);
#endif
#ifndef NO_MIN
        InterlockedMin(slots[0][1], id.x, least);
#endif
    }
    else
    {
#ifndef NO_ADD
        InterlockedAdd(slots[1][0], 1u, added);
#endif
#ifndef NO_MIN
        InterlockedMin(slots[1][1], id.x, least);
#endif
    }
#else
#ifndef NO_ADD
    InterlockedAdd(AT(parity)[0], 1u, added);
#endif
#ifndef NO_MIN
    InterlockedMin(AT(parity)[1], id.x, least);
#endif
#endif
#ifdef USE_RESULT
    results[id.x * 2] = added;
    results[id.x * 2 + 1] = least;
#endif
}
