// A mesh stage alone: one vertex and no triangle, so nothing rasterizes and no pixel stage runs.
// With -D TABLE=1 its x is read from a constant table at the lane's index, which DXC states as an internal constant;
// with -D TABLE=0 it is a select of the same two values.

#ifndef TABLE
#define TABLE 1
#endif

struct vertex_out
{
    float4 position : SV_Position;
};

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void ms(out vertices vertex_out verts[1], out indices uint3 tris[1])
{
    SetMeshOutputCounts(1, 0);
#if TABLE
    float const xs[2] = {0.25, 0.5};
    verts[0].position = float4(xs[WaveGetLaneIndex() & 1], 0, 0.5, 1);
#else
    verts[0].position = float4((WaveGetLaneIndex() & 1) != 0 ? 0.5 : 0.25, 0, 0.5, 1);
#endif
}
