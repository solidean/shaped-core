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

// A ray payload, for the generated-size half of the test. Nothing traces it here -- what is under test is
// that the mirror and max_payload_size come out of the same parse the shader did.
#pragma sc payload
struct trace_payload
{
    float2 uv;
    float3 radiance;
    uint depth;
};
