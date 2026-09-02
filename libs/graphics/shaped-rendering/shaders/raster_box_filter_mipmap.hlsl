// Fills one mip level from the level above it with the same 4-tap box filter box_filter_mipmap.hlsl applies,
// through the raster pipeline rather than through a UAV.
//
// This is the path for a format that cannot carry a typed unordered-access view, which in practice means an sRGB
// one: sampling decodes the transfer function and a render-target write encodes it again, so the average is taken
// over linear values.
// A UAV write defines neither conversion, which is why the format is refused there rather than merely discouraged.
//
// One entry point, for 2D non-array textures.
// Every other shape stays on the compute path — a render target is 2D-shaped, and covering an array would mean one
// pass per slice rather than one dispatch for all of them.
//
// One pass per level, with the source bound as a single-mip view of level N and the target as the render-target
// view of N+1, so no level is ever read and written by the same pass.

#include "sc/portable.hlsli"

#define SC_GROUP 0
SC_BINDING Texture2D<float4> gSource;
#undef SC_GROUP

struct vs_output
{
    float4 pos : SV_Position;
};

// No vertex input — the covering triangle is generated from SV_VertexID (draw 3 vertices).
vs_output main_vs(uint vid : SV_VertexID)
{
    vs_output o;
    float2 uv = float2((vid << 1) & 2, vid & 2); // {(0,0),(2,0),(0,2)} — a screen-covering triangle
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 main_ps(vs_output i) : SV_Target
{
    // SV_Position is the pixel center, so truncating gives this texel's integer coordinate in the target level.
    uint2 base = uint2(i.pos.xy) * 2;

    uint2 source_size;
    gSource.GetDimensions(source_size.x, source_size.y);

    // A source extent that is odd on some axis means the level is not exactly twice the target there; clamping the
    // second tap keeps it inside the level instead of sampling the border.
    uint2 hi = min(base + 1, source_size - 1);

    float4 sum = gSource.Load(int3(base.x, base.y, 0)) + gSource.Load(int3(hi.x, base.y, 0))
                 + gSource.Load(int3(base.x, hi.y, 0)) + gSource.Load(int3(hi.x, hi.y, 0));

    return sum * 0.25;
}
