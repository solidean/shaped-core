// Two triangles covering the target, carrying what pixel.hlsl reads: its instance, the primitive and a world position.
// The world position is the one a 4 x 4 quad at z = 0 has, seen from z = -3.4 through a 45 degree field of view.

struct varyings
{
    float4 position : SV_Position;
    nointerpolation uint instance : SGL0;
    nointerpolation uint primitive : SGL1;
    float3 barycentrics : SGL2;
    float3 world : SGL3;
};

varyings main_vs(uint vertex : SV_VertexID)
{
    const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
    const float2 c = corners[vertex % 6];
    varyings o;
    o.position = float4(c * 3.4, 0.1, 3.4);
    o.instance = 0;
    o.primitive = vertex / 3;
    o.barycentrics = float3(1, 0, 0);
    o.world = float3(c * 3.4 * 0.41421356, 0);
    return o;
}
