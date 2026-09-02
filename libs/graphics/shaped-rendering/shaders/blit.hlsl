// Fullscreen-triangle blit: sample the ray-traced view target onto a window back buffer.
// No vertex input — the covering triangle is generated from SV_VertexID (draw 3 vertices).

#include "sc/portable.hlsli"

#define SC_GROUP 0
SC_BINDING Texture2D<float4> source_texture;
SC_BINDING SamplerState linear_sampler;
#undef SC_GROUP

struct vs_output
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

vs_output main_vs(uint vid : SV_VertexID)
{
    vs_output o;
    o.uv = float2((vid << 1) & 2, vid & 2); // {(0,0),(2,0),(0,2)} — a screen-covering triangle
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 main_ps(vs_output i) : SV_Target
{
    return source_texture.SampleLevel(linear_sampler, i.uv, 0);
}
