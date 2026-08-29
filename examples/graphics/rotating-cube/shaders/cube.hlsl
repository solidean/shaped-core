// One cube, six colored faces, two fixed lights.
//
// Written for both backends from one source, which is what makes the two annotations below worth reading.
//
// `[[vk::location(N)]]` numbers a vertex input. sg identifies an attribute by its HLSL semantic, and SPIR-V has no
// semantics at all — so a Vulkan-targeted shader has to spell out the locations, in the order the sg vertex layout
// lists its attributes. Getting that order wrong is silent: the pipeline builds and the geometry is wrong.
//
// `[[vk::push_constant]]` is what makes the constant block sg's inline constants rather than a descriptor.
// The `__spirv__` fork is DXC's own: the attribute has no DXIL meaning, and a plain `register(b0)` is what a root
// constant looks like there.

struct vs_input
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float3 color : COLOR;
};

struct vs_output
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

struct cube_constants
{
    float4x4 view_projection;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<cube_constants> gConstants;
#else
ConstantBuffer<cube_constants> gConstants : register(b0);
#endif

vs_output main_vs(vs_input input)
{
    vs_output output;
    output.position = mul(gConstants.view_projection, float4(input.position, 1.0f));
    output.normal = input.normal;
    output.color = input.color;
    return output;
}

float4 main_ps(vs_output input) : SV_Target
{
    // A key light, a fill light and an ambient floor — enough that a face's orientation reads at a glance, and
    // little enough that nothing here needs explaining.
    float3 n = normalize(input.normal);
    float key = saturate(dot(n, normalize(float3(0.45f, 0.8f, -0.4f))));
    float fill = saturate(dot(n, normalize(float3(-0.7f, 0.15f, 0.6f))));

    float3 lit = input.color * (0.25f + 0.8f * key + 0.25f * fill);
    return float4(lit, 1.0f);
}
