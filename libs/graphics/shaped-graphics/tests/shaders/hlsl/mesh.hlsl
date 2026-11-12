// Shader fixture for mesh-test.cc: a mesh stage that writes a full-screen triangle, and a task stage that launches two
// mesh workgroups, each covering half of the target, with one payload.

struct mesh_vertex
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

// One workgroup, one triangle over the whole target, green.
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void ms_main(out vertices mesh_vertex verts[3], out indices uint3 tris[1])
{
    SetMeshOutputCounts(3, 1);
    const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    for (uint i = 0; i < 3; ++i)
    {
        verts[i].position = float4(positions[i], 0.0, 1.0);
        verts[i].color = float4(0.0, 1.0, 0.0, 1.0);
    }
    tris[0] = uint3(0, 1, 2);
}

struct halves
{
    float blue;
};

groupshared halves g_halves;

// Two mesh workgroups, each handed the same blue.
[numthreads(1, 1, 1)]
void as_main()
{
    g_halves.blue = 1.0;
    DispatchMesh(2, 1, 1, g_halves);
}

// Workgroup 0 covers the left half and workgroup 1 the right one, red by its index and blue by the payload.
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void ms_halves(uint3 group : SV_GroupID, in payload halves p, out vertices mesh_vertex verts[4], out indices uint3 tris[2])
{
    SetMeshOutputCounts(4, 2);
    const float left = group.x == 0 ? -1.0 : 0.0;
    const float2 corners[4] = {float2(left, -1.0), float2(left + 1.0, -1.0), float2(left + 1.0, 1.0), float2(left, 1.0)};
    for (uint i = 0; i < 4; ++i)
    {
        verts[i].position = float4(corners[i], 0.0, 1.0);
        verts[i].color = float4(float(group.x), 0.0, p.blue, 1.0);
    }
    tris[0] = uint3(0, 1, 2);
    tris[1] = uint3(0, 2, 3);
}

float4 ps_main(mesh_vertex v) : SV_Target
{
    return v.color;
}
