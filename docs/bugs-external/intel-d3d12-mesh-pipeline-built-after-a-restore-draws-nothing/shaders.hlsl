// One full-screen green triangle through a mesh stage, and a pixel stage that passes its color on.

struct corner
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void ms(out vertices corner v[3], out indices uint3 i[1])
{
    SetMeshOutputCounts(3, 1);
    float4 green = float4(0, 1, 0, 1);
    v[0].position = float4(-1, -1, 0, 1);
    v[0].color = green;
    v[1].position = float4(3, -1, 0, 1);
    v[1].color = green;
    v[2].position = float4(-1, 3, 0, 1);
    v[2].color = green;
    i[0] = uint3(0, 1, 2);
}

float4 ps(corner c) : SV_Target
{
    return c.color;
}

corner vs(uint id : SV_VertexID)
{
    corner c;
    c.position = float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0, 1);
    c.color = float4(0, 1, 0, 1);
    return c;
}
