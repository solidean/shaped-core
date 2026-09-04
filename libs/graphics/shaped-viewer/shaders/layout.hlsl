// The one shader that draws a layout: the frame's chrome, and every view placed where the layout put it.
//
// One entry point per draw kind rather than an uber-shader branch, so each gets its own pipeline through the routine's
// keyed cache. The covering triangle is generated from SV_VertexID (draw 3 vertices) and the caller's viewport is what
// confines it to the draw's rect — nothing here knows a rect.
//
// Every sv view target holds PREMULTIPLIED alpha: straight alpha is not associative across a nesting chain, and a
// three-level composite would visibly darken edges.
//
// Every address below is written by slib's binding pass; see shaped-shader-library/docs/binding-preprocessor.md.
//
// The constants block is declared in the vertex stage only, and everything the pixel stages need arrives as a varying.
// That was forced by the reflected group layout, which the declared group no longer is — a push_constants block is
// not a group member whatever reads it, so the three nointerpolation varyings could now be dropped.
// Left alone here: it is a change to what the stages interpolate, not to who owns an address.

struct layout_constants
{
    float4 uv_scale_bias_0; // xy scale, zw bias — the primary source's sub-rect
    float4 uv_scale_bias_1; // the wipe's second source
    float4 tint;            // multiplied into the sample; also the border's flat color
    float4 wipe;            // x split in [0,1], y axis (0 = horizontal), z separator half-width in uv, w unused
    float4 separator_color; // the wipe's seam band
};

#pragma sc push_constants space=0
ConstantBuffer<layout_constants> constants;

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

// The sampler is dynamic rather than `static`: a draw picks nearest or linear, so its state is the caller's.
#pragma sc group 0
namespace layout_bindings
{
    Texture2D<float4> source_0;
    Texture2D<float4> source_1;
    SamplerState source_sampler;
}

vs_output main_vs(uint vid : SV_VertexID)
{
    // {(0,0),(2,0),(0,2)} — a triangle covering the viewport the caller set to this draw's rect.
    float2 t = float2((vid << 1) & 2, vid & 2);

    vs_output o;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv0 = t * constants.uv_scale_bias_0.xy + constants.uv_scale_bias_0.zw;
    o.uv1 = t * constants.uv_scale_bias_1.xy + constants.uv_scale_bias_1.zw;
    o.local = t;
    o.tint = constants.tint;
    o.wipe = constants.wipe;
    o.separator = constants.separator_color;
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
    return layout_bindings::source_0.SampleLevel(layout_bindings::source_sampler, i.uv0, 0) * i.tint;
}

// Two views split along an axis, with an optional separator band on the split itself.
float4 wipe_ps(vs_output i) : SV_Target
{
    float along = i.wipe.y < 0.5 ? i.local.x : i.local.y;

    float split = i.wipe.x;
    float4 a = layout_bindings::source_0.SampleLevel(layout_bindings::source_sampler, i.uv0, 0);
    float4 b = layout_bindings::source_1.SampleLevel(layout_bindings::source_sampler, i.uv1, 0);
    float4 c = along < split ? a : b;

    // The separator is drawn opaque over the seam; a zero half-width draws none.
    float half_width = i.wipe.z;
    if (half_width > 0 && abs(along - split) < half_width)
        c = i.separator;

    return c * i.tint;
}
