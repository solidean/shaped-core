// One workgroup of four invocations.
// Each invocation but #2 sets its own bit of a groupshared word; after a barrier, every invocation writes back the word it
// sees, which must be 0xb.
// `aborted` is never true, since k < 4 always: it is set in a guard that is never taken, gates the mark, and gates a
// return after the read.
//
// LATE_RETURN=1 returns on `aborted` after the barrier: on NVIDIA the marks of #1 and #3 are lost and every word is 0x1.
// LATE_RETURN=0 returns on the same condition spelled again (k >= 4): every word is 0xb, the control.
// The compute stage (-T cs_6_5 -E cs) runs the same body and keeps every mark.

RWStructuredBuffer<uint> result : register(u0);

groupshared uint marks;

struct vertex
{
    float4 position : SV_Position;
};

bool mark_and_read(uint k)
{
    if (k == 0)
        marks = 0;
    GroupMemoryBarrierWithGroupSync();
    bool aborted = false;
    if (k != 2)
    {
        if (k >= 4)
            aborted = true;
        if (!aborted)
            InterlockedOr(marks, 1u << k);
    }
    GroupMemoryBarrierWithGroupSync();
    uint seen;
    InterlockedOr(marks, 0u, seen);
    result[k] = seen;
    return aborted;
}

[outputtopology("triangle")]
[numthreads(4, 1, 1)]
void ms(uint k : SV_GroupIndex, out vertices vertex verts[4], out indices uint3 tris[2])
{
    bool aborted = mark_and_read(k);
    SetMeshOutputCounts(4, 2);
#if LATE_RETURN
    if (aborted)
        return;
#else
    if (k >= 4)
        return;
#endif
    verts[k].position = float4(float(k & 1) * 2.0 - 1.0, float(k >> 1) * 2.0 - 1.0, 0.0, 1.0);
    if (k == 0)
    {
        tris[0] = uint3(0, 1, 2);
        tris[1] = uint3(1, 3, 2);
    }
}

[numthreads(4, 1, 1)]
void cs(uint k : SV_GroupIndex)
{
    mark_and_read(k);
}
