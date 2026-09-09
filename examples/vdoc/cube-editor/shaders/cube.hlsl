// One instanced draw for every cube in the document.
//
// Slot 0 is the unit cube: eight corners' worth of geometry expanded to 24 vertices, because a shared corner
// carries three different normals and a normal is what makes the faces distinguishable at all.
// Slot 1 is per-instance, one entry per entity, rebuilt every frame from the parsed document.
//
// The view-projection rides as inline (root) constants: 64 bytes, rewritten once per frame, which is exactly what
// they are for. Everything varying per cube is in the instance stream instead.
//
// Every address here belongs to slib's binding pass rather than to this file — see
// libs/graphics/shaped-shader-library/docs/binding-preprocessor.md.
// The two streams are two annotated structs rather than one, which is what says which buffer feeds which member:
// a `vertex_input` describes exactly one bound slot, and the slots are its declaration order.

#pragma sc vertex_input
struct vs_input
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

#pragma sc vertex_input per_instance
struct instance_input
{
    float3 center : TEXCOORD0;
    float3 half_extent : TEXCOORD1;
    float3 color : TEXCOORD2;
    float highlight : TEXCOORD3;
};

struct vs_output
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR;
    float highlight : TEXCOORD0;
};

// `column_major` is stated rather than left to the default, and it is load-bearing twice over.
// It decides the layout, which is what the generated mirror reproduces -- and it decides whether those sixteen
// floats are read as rows or as columns, which is what `mul` does with them.
// The default it would otherwise take comes from `#pragma pack_matrix` / `-Zpr`, so without this word a flag set
// anywhere could transpose the cube with the mirror's size still correct.
struct cube_constants
{
    column_major float4x4 view_projection;
};

#pragma sc push_constants
ConstantBuffer<cube_constants> gConstants;

vs_output main_vs(vs_input input, instance_input inst)
{
    float3 world = inst.center + input.position * inst.half_extent;

    vs_output output;
    output.position = mul(gConstants.view_projection, float4(world, 1.0f));
    output.normal = input.normal;
    output.color = inst.color;
    output.highlight = inst.highlight;
    return output;
}

float4 main_ps(vs_output input) : SV_Target
{
    // Two fixed lights and a floor bounce, so orientation reads without a lighting model worth explaining.
    float3 n = normalize(input.normal);
    float key = saturate(dot(n, normalize(float3(0.4f, 0.85f, -0.3f))));
    float fill = saturate(dot(n, normalize(float3(-0.6f, 0.2f, 0.7f))));
    float3 lit = input.color * (0.28f + 0.75f * key + 0.22f * fill);

    // Selection is a brightening rather than an outline: no second pipeline, and it survives any camera angle.
    lit = lerp(lit, saturate(lit * 1.6f + 0.22f), input.highlight);

    return float4(lit, 1.0f);
}
