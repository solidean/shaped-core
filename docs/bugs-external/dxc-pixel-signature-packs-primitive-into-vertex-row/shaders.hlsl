// One mesh group writes a quad over the whole target as two triangles, each with a per-primitive `weight` of its own.
// Every vertex also carries `id`; the pixel stage reads both and writes 1 to the second target's alpha.
// With -D SHARED=1 `id` is nointerpolation, so DXC packs the pixel stage's `weight` into `id`'s row, which the mesh
// spec forbids; with -D SHARED=0 it is linear, which no primitive input can share a row with.

#ifndef SHARED
#define SHARED 1
#endif

#if SHARED
#define ID_MODE nointerpolation
#else
#define ID_MODE
#endif

struct vertex_out
{
    float4 position : SV_Position;
    ID_MODE float id : ID;
};

struct primitive_out
{
    float2 weight : WEIGHT;
};

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void ms(out vertices vertex_out verts[4], out indices uint3 tris[2], out primitives primitive_out prims[2])
{
    SetMeshOutputCounts(4, 2);
    // written one by one: a table of the corners hangs Intel's Arc, intel-d3d12-mesh-table-without-root-parameters-hangs
    verts[0].position = float4(-1, -1, 0.5, 1);
    verts[1].position = float4(1, -1, 0.5, 1);
    verts[2].position = float4(1, 1, 0.5, 1);
    verts[3].position = float4(-1, 1, 0.5, 1);
    for (uint v = 0; v < 4; ++v)
        verts[v].id = float(v);
    for (uint t = 0; t < 2; ++t)
    {
        tris[t] = uint3(0, t + 1, t + 2);
        prims[t].weight = float2(t + 0.5, 3.0);
    }
}

struct targets
{
    float4 first : SV_Target0;
    float4 second : SV_Target1;
};

struct primitive_in
{
    nointerpolation float2 weight : WEIGHT;
};

targets ps(vertex_out v, primitive_in p)
{
    targets t;
    t.first = float4(v.id, 0, 0, 0);
    t.second = float4(p.weight, 0, 1);
    return t;
}
