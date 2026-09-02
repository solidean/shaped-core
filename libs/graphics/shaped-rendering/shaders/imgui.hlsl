// Dear ImGui's draw lists, in one triangle-list pass.
// Paired with sr::imgui_draw_routine.
//
// Positions arrive in imgui's display space (pixels, origin top-left) and reach clip space through the inline constants below.
// imgui only ever needs a 2D ortho projection, so scale + translate is the whole transform — a full mat4 would be 48 further bytes of root constants carrying zeros.
//
// Colors are sRGB-encoded 8-bit and must reach the target unconverted, so the target must not be an _srgb format.
// The routine asserts that; see its header for why compensating here would be worse.

#include "sc/portable.hlsli"

struct vs_input
{
    SC_VERTEX_INPUT(0) float2 position : POSITION;
    SC_VERTEX_INPUT(1) float2 uv       : TEXCOORD;
    SC_VERTEX_INPUT(2) float4 color    : COLOR;
};

struct vs_output
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
};

// Inline (root/push) constants — 16 bytes, rewritten once per frame.
// Excluded from the binding group; the routine passes this binding as pipeline_layout_description::inline_constants.
struct imgui_constants
{
    float2 scale;
    float2 translate;
};
SC_INLINE_CONSTANTS(imgui_constants, gConstants);

vs_output main_vs(vs_input input)
{
    vs_output output;
    output.position = float4(input.position * gConstants.scale + gConstants.translate, 0.0f, 1.0f);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

// Binding group 0 (space0).
// gSampler is name-matched as a *static* sampler on the group layout, so it costs no per-group descriptor and never varies frame to frame.
#define SC_GROUP 0
SC_BINDING Texture2D gTexture;
SC_BINDING SamplerState gSampler;
#undef SC_GROUP

float4 main_ps(vs_output input) : SV_Target
{
    return input.color * gTexture.Sample(gSampler, input.uv);
}
