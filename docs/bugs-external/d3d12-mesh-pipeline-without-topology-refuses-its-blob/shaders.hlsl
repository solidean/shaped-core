// The two stages repro.cc builds pipelines of: a mesh stage of one triangle, and a vertex stage for the control row.

struct vertex
{
    float4 position : SV_Position;
};

[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void ms(out vertices vertex v[3], out indices uint3 i[1])
{
    SetMeshOutputCounts(3, 1);
    v[0].position = float4(-1, -1, 0, 1);
    v[1].position = float4(3, -1, 0, 1);
    v[2].position = float4(-1, 3, 0, 1);
    i[0] = uint3(0, 1, 2);
}

float4 vs(uint id : SV_VertexID) : SV_Position
{
    return float4(float(id), 0, 0, 1);
}
