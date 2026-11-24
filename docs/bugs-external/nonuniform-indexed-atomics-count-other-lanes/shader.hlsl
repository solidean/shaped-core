// 64 threads; thread i updates element i % 2 of a two-element array of buffers.
// Slot 0 of each buffer counts the threads of its parity, slot 1 keeps the least of their ids.
// Expected: even = {32, 0}, odd = {32, 1}.
//
// Defines, each one variable of the matrix.
// The bug:
//   NO_ADD, NO_MIN       leave that atomic out
//   USE_RESULT           write each atomic's previous value to `results`, so the shader reads it
//   BRANCH               select the element by a branch on the parity, with literal indices, instead of by index
// Workarounds, each picking the buffer by index; every one is right on WARP:
//   BREAK_LOOP           one round per distinct index of the wave, its lanes' atomics in the round, leaving by `break`
//   FLAG_LOOP            the same rounds, leaving by a flag
//   FLAG_LOOP_UNMARKED   as FLAG_LOOP, the round's index without NonUniformResourceIndex, as it is uniform there
//   FLAG_LOOP_ONE_BUFFER as FLAG_LOOP, every round updating the even buffer, so no round picks another
//   LEADER               the rounds take wave reductions alone; after the loop, one lane per index does the atomic
//   LEADER_CAS           as LEADER, the atomic a compare-exchange loop
//   LANE_CAS             no rounds: each lane's own compare-exchange loop
//   TURN_CAS             as LANE_CAS, one lane at a time: the first lane still waiting runs its loop alone
//   LITERAL_CAS          the add alone, each lane's compare-exchange loop on a buffer picked by a literal index
//   ONE_BUFFER_CAS       every lane's compare-exchange loop on `results`, no binding array at all
//   TURN_NATIVE          one lane at a time, each its native atomics alone
//   ROUND_TURN_NATIVE    as TURN_NATIVE, within a round per distinct index
//   ROUND_TURN_CAS       as TURN_CAS, within a round per distinct index, so every lane waiting holds one index
//   ROUND_CAS            the add alone, by a leader in its round, each lane given its own previous value;
//                        slot 1, all ones, then loses each previous value's bit, so it is 0 where the lanes got 0 to 31
//   ROUND_NATIVE         as ROUND_CAS, the leader's update the native atomic
//   SHUFFLE_CAS          as ROUND_CAS, the leader's compare-exchange after the loop, its value shuffled back

RWStructuredBuffer<uint> slots[2] : register(u0);
RWStructuredBuffer<uint> results : register(u2);

#define AT(i) slots[NonUniformResourceIndex(i)]

#if defined(FLAG_LOOP_UNMARKED)
#define ROUND(i) slots[i]
#elif defined(FLAG_LOOP_ONE_BUFFER)
#define ROUND(i) slots[0]
#else
#define ROUND(i) AT(i)
#endif

#if defined(FLAG_LOOP_UNMARKED) || defined(FLAG_LOOP_ONE_BUFFER)
#define FLAG_LOOP
#endif
#ifdef LEADER_CAS
#define LEADER
#endif

// `*at += value` by compare-exchange, returning the previous value
#define CAS_ADD(at, value, previous)                                  \
    {                                                                 \
        uint seen = at;                                               \
        for (;;)                                                      \
        {                                                             \
            uint original;                                            \
            InterlockedCompareExchange(at, seen, seen + value, original); \
            if (original == seen)                                     \
                break;                                                \
            seen = original;                                          \
        }                                                             \
        previous = seen;                                              \
    }

// `*at = min(*at, value)` by compare-exchange, returning the previous value
#define CAS_MIN(at, value, previous)                              \
    {                                                             \
        uint seen = at;                                           \
        while (value < seen)                                      \
        {                                                         \
            uint original;                                        \
            InterlockedCompareExchange(at, seen, value, original); \
            if (original == seen)                                 \
                break;                                            \
            seen = original;                                      \
        }                                                         \
        previous = seen;                                          \
    }

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint const parity = id.x % 2;
    uint added = 0;
    uint least = 0;
#if defined(ROUND_CAS) || defined(ROUND_NATIVE) || defined(SHUFFLE_CAS)
    // the add alone, its previous value each lane's own: the leader's plus the lane's place among its round's lanes
    uint leader_lane = 0;
    uint prefix = 0;
    uint count = 0;
    uint before = 0;
    for (;;)
    {
        uint const first = WaveReadLaneFirst(parity);
        if (parity == first)
        {
            count = WaveActiveCountBits(true);
            prefix = WavePrefixCountBits(true);
#if defined(ROUND_CAS)
            if (WaveIsFirstLane())
                CAS_ADD(AT(parity)[0], count, before)
            before = WaveReadLaneFirst(before);
#elif defined(ROUND_NATIVE)
            if (WaveIsFirstLane())
                InterlockedAdd(AT(parity)[0], count, before);
            before = WaveReadLaneFirst(before);
#else
            leader_lane = firstbitlow(WaveActiveBallot(true).x);
#endif
            break;
        }
    }
#if defined(SHUFFLE_CAS)
    if (prefix == 0)
        CAS_ADD(AT(parity)[0], count, before)
    before = WaveReadLaneAt(before, leader_lane);
#endif
    added = before + prefix;
    // each lane clears its previous value's bit, by a branch to literal indices, which is right everywhere
    if (parity == 0)
        InterlockedAnd(slots[0][1], ~(1u << (added & 31u)));
    else
        InterlockedAnd(slots[1][1], ~(1u << (added & 31u)));
#elif defined(LEADER)
    uint count = 0;
    uint low = 0;
    bool leader = false;
    for (;;)
    {
        uint const first = WaveReadLaneFirst(parity);
        if (parity == first)
        {
            count = WaveActiveCountBits(true);
            low = WaveActiveMin(id.x);
            leader = WaveIsFirstLane();
            break;
        }
    }
    if (leader)
    {
#if defined(LEADER_CAS)
#ifndef NO_ADD
        CAS_ADD(AT(parity)[0], count, added)
#endif
#ifndef NO_MIN
        CAS_MIN(AT(parity)[1], low, least)
#endif
#else
#ifndef NO_ADD
        InterlockedAdd(AT(parity)[0], count, added);
#endif
#ifndef NO_MIN
        InterlockedMin(AT(parity)[1], low, least);
#endif
#endif
    }
#elif defined(LITERAL_CAS)
    if (parity == 0)
    {
        CAS_ADD(slots[0][0], 1u, added)
    }
    else
    {
        CAS_ADD(slots[1][0], 1u, added)
    }
#elif defined(ONE_BUFFER_CAS)
    CAS_ADD(results[0], 1u, added)
#elif defined(TURN_NATIVE)
    for (;;)
    {
        if (WaveIsFirstLane())
        {
#ifndef NO_ADD
            InterlockedAdd(AT(parity)[0], 1u, added);
#endif
#ifndef NO_MIN
            InterlockedMin(AT(parity)[1], id.x, least);
#endif
            break;
        }
    }
#elif defined(ROUND_TURN_NATIVE)
    for (;;)
    {
        uint const first = WaveReadLaneFirst(parity);
        if (parity == first)
        {
            for (;;)
            {
                if (WaveIsFirstLane())
                {
#ifndef NO_ADD
                    InterlockedAdd(AT(parity)[0], 1u, added);
#endif
#ifndef NO_MIN
                    InterlockedMin(AT(parity)[1], id.x, least);
#endif
                    break;
                }
            }
            break;
        }
    }
#elif defined(ROUND_TURN_CAS)
    for (;;)
    {
        uint const first = WaveReadLaneFirst(parity);
        if (parity == first)
        {
            for (;;)
            {
                if (WaveIsFirstLane())
                {
#ifndef NO_ADD
                    CAS_ADD(AT(parity)[0], 1u, added)
#endif
#ifndef NO_MIN
                    CAS_MIN(AT(parity)[1], id.x, least)
#endif
                    break;
                }
            }
            break;
        }
    }
#elif defined(TURN_CAS)
    for (;;)
    {
        if (WaveIsFirstLane())
        {
#ifndef NO_ADD
            CAS_ADD(AT(parity)[0], 1u, added)
#endif
#ifndef NO_MIN
            CAS_MIN(AT(parity)[1], id.x, least)
#endif
            break;
        }
    }
#elif defined(LANE_CAS)
#ifndef NO_ADD
    CAS_ADD(AT(parity)[0], 1u, added)
#endif
#ifndef NO_MIN
    CAS_MIN(AT(parity)[1], id.x, least)
#endif
#elif defined(BREAK_LOOP)
    for (;;)
    {
        uint const first = WaveReadLaneFirst(parity);
        if (parity == first)
        {
#ifndef NO_ADD
            InterlockedAdd(ROUND(first)[0], 1u, added);
#endif
#ifndef NO_MIN
            InterlockedMin(ROUND(first)[1], id.x, least);
#endif
            break;
        }
    }
#elif defined(FLAG_LOOP)
    bool done = false;
    while (!done)
    {
        uint const first = WaveReadLaneFirst(parity);
        if (parity == first)
        {
#ifndef NO_ADD
            InterlockedAdd(ROUND(first)[0], 1u, added);
#endif
#ifndef NO_MIN
            InterlockedMin(ROUND(first)[1], id.x, least);
#endif
            done = true;
        }
    }
#elif defined(BRANCH)
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
