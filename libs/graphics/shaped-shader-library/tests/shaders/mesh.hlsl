// A vertex input struct and the pass-through stages that read it, for the generated-mirror half of the test.
#pragma sc vertex_input
struct vs_input
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
};

#pragma sc vertex_input slot=1 per_instance
struct instance_input
{
    float3 center : TEXCOORD0;
    uint tint : TEXCOORD1;
};

struct vs_output
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

vs_output main_vs(vs_input v, instance_input i)
{
    vs_output o;
    o.position = float4(v.position + i.center, 1.0f);
    o.color = v.color * float(i.tint);
    return o;
}

float4 main_ps(vs_output i) : SV_Target
{
    return i.color;
}
