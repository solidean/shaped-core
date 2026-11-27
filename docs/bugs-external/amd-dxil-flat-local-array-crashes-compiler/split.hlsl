// flat.hlsl's program over an array of structs, which DXC splits into sixteen `[8 x float]`, one per field and lane.
StructuredBuffer<uint> params : register(t0);
RWStructuredBuffer<float4> target : register(u1);
struct Vertex
{
    float4 a, b, c, d;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Vertex zero = {float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0)};
    Vertex q[8];
    [unroll] for (int k = 0; k < 8; ++k)
        q[k] = zero;
    if (params[0] == 0u)
    {
        [loop] for (int z = 0; z < 8; ++z)
            q[z] = zero;
    }
    uint n = params[1];
    [loop] for (int g = 0; g < int(n); ++g)
        q[g].d.w = 0.5;
    [loop] for (int i = 0; i < int(n); ++i)
    {
        target[id.x * 32 + i * 4 + 0] = q[i].a;
        target[id.x * 32 + i * 4 + 1] = q[i].b;
        target[id.x * 32 + i * 4 + 2] = q[i].c;
        target[id.x * 32 + i * 4 + 3] = q[i].d;
    }
}
