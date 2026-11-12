// One SetMeshOutputCounts in uniform control flow, between two branches on the same signed value.
// Each macro changes one ingredient; the defaults are the failing shader, which run.py's matrix varies.

#ifndef FIRST_BRANCH
#define FIRST_BRANCH 1 // 0: no branch before the call
#endif
#ifndef FIRST_AXIS
#define FIRST_AXIS x // y: the first branch tests a value the second does not
#endif
#ifndef FIRST_VALUE
#define FIRST_VALUE 1 // any value but 0 says the second branch's outcome on the first's path
#endif
#ifndef SIGNED
#define SIGNED 1 // 0: both branches test the uint SV_GroupID itself
#endif

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(uint3 group_id : SV_GroupID, out vertices float4 positions[3] : SV_Position, out indices uint3 triangles[1])
{
#if SIGNED
    int3 const group = int3(group_id);
#else
    uint3 const group = group_id;
#endif
#if FIRST_BRANCH
    if (group.FIRST_AXIS == FIRST_VALUE)
        AllMemoryBarrier();
#endif
    SetMeshOutputCounts(3, 1);
    float left;
    if (group.x == 0)
        left = -1.0;
    else
        left = 0.0;
    positions[0] = float4(left, 0.0, 0.0, 1.0);
    triangles[0] = uint3(0, 1, 2);
}
