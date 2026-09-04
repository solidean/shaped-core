// Dear ImGui's draw lists, in one triangle-list pass.
// Paired with sr::imgui_draw_routine.
//
// Positions arrive in imgui's display space (pixels, origin top-left) and reach clip space through the inline constants below.
// imgui only ever needs a 2D ortho projection, so scale + translate is the whole transform — a full mat4 would be 48 further bytes of root constants carrying zeros.
//
// Colors are sRGB-encoded 8-bit and must reach the target unconverted, so the target must not be an _srgb format.
// The routine asserts that; see its header for why compensating here would be worse.
//
// Every address below is written by slib's binding pass rather than by hand; see shaped-shader-library/docs/binding-preprocessor.md.

struct vs_input
{
    float2 position : POSITION;
    float2 uv       : TEXCOORD;
    float4 color    : COLOR;
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

#pragma sc push_constants space=9
ConstantBuffer<imgui_constants> gConstants;

vs_output main_vs(vs_input input)
{
    vs_output output;
    output.position = float4(input.position * gConstants.scale + gConstants.translate, 0.0f, 1.0f);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

// The sampler is declared `static`, so it is baked into the group layout and costs no per-group descriptor.
// Clamp-to-edge stops the atlas bleeding across glyph edges.
#pragma sc group 0
namespace imgui_bindings
{
    Texture2D<float4> texture;
#pragma sc static address=clamp_edge
    SamplerState texture_sampler;
}

float4 main_ps(vs_output input) : SV_Target
{
    return input.color * imgui_bindings::texture.Sample(imgui_bindings::texture_sampler, input.uv);
}
