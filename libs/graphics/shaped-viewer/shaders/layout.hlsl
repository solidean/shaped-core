// The one shader that draws a layout: the frame's chrome, and every view placed where the layout put it.
//
// One entry point per draw kind rather than an uber-shader branch, so each gets its own pipeline through the routine's
// keyed cache. The covering triangle is generated from SV_VertexID (draw 3 vertices) and the caller's viewport is what
// confines it to the draw's rect — nothing here knows a rect.
//
// Every sv view target holds PREMULTIPLIED alpha: straight alpha is not associative across a nesting chain, and a
// three-level composite would visibly darken edges.
//
// The constants block is declared in the vertex stage only, and everything the pixel stages need arrives as a varying.
// Declaring it in a pixel stage too would put it back into the reflected group layout, which pipeline_layout.hh forbids
// for inline constants.

cbuffer layout_constants : register(b0)
{
    float4 uv_scale_bias_0; // xy scale, zw bias — the primary source's sub-rect
    float4 uv_scale_bias_1; // the wipe's second source
    float4 tint;            // multiplied into the sample; also the border's flat color
    float4 wipe;            // x split in [0,1], y axis (0 = horizontal), z separator half-width in uv, w unused
    float4 separator_color; // the wipe's seam band
};

struct vs_output
{
    float4 pos : SV_Position;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;

    /// The draw-local [0,1] coordinate, before any source sub-rect.
    /// A wipe's split must sit where the leaf is on screen, whatever each side happens to sample — and carrying it as
    /// a varying is what keeps the constants block out of the pixel stages, and so out of the reflected group layout.
    float2 local : TEXCOORD2;

    nointerpolation float4 tint : TEXCOORD3;
    nointerpolation float4 wipe : TEXCOORD4;
    nointerpolation float4 separator : TEXCOORD5;
};

Texture2D<float4> source_0 : register(t0);
Texture2D<float4> source_1 : register(t1);
SamplerState source_sampler : register(s0);

vs_output main_vs(uint vid : SV_VertexID)
{
    // {(0,0),(2,0),(0,2)} — a triangle covering the viewport the caller set to this draw's rect.
    float2 t = float2((vid << 1) & 2, vid & 2);

    vs_output o;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv0 = t * uv_scale_bias_0.xy + uv_scale_bias_0.zw;
    o.uv1 = t * uv_scale_bias_1.xy + uv_scale_bias_1.zw;
    o.local = t;
    o.tint = tint;
    o.wipe = wipe;
    o.separator = separator_color;
    return o;
}

// A border band: one flat color over the viewport.
float4 border_ps(vs_output i) : SV_Target
{
    return i.tint;
}

// One view across its rect.
float4 view_ps(vs_output i) : SV_Target
{
    return source_0.SampleLevel(source_sampler, i.uv0, 0) * i.tint;
}

// Two views split along an axis, with an optional separator band on the split itself.
float4 wipe_ps(vs_output i) : SV_Target
{
    float along = i.wipe.y < 0.5 ? i.local.x : i.local.y;

    float split = i.wipe.x;
    float4 a = source_0.SampleLevel(source_sampler, i.uv0, 0);
    float4 b = source_1.SampleLevel(source_sampler, i.uv1, 0);
    float4 c = along < split ? a : b;

    // The separator is drawn opaque over the seam; a zero half-width draws none.
    float half_width = i.wipe.z;
    if (half_width > 0 && abs(along - split) < half_width)
        c = i.separator;

    return c * i.tint;
}
