// One cube, six colored faces, two fixed lights.
//
// Written for both backends from one source, and every address in it belongs to slib's binding pass rather than
// to this file — see libs/graphics/shaped-shader-library/docs/binding-preprocessor.md.
//
// This shader used to fork on `__spirv__` twice, which is the fork the pass exists to remove.
// A vertex input needed `[[vk::location(N)]]` because SPIR-V has no semantics, and getting the order wrong was
// silent: the pipeline built and the geometry was wrong.
// The constant block needed `[[vk::push_constant]]` on one target and `register(b0)` on the other.
// Both are now one attribute apiece, resolved per target by the pass.

#pragma sc vertex_input
struct vs_input
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

struct vs_output
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

// The matrix is declared bare, and the pass writes `column_major` in front of it before the compiler sees this
// — the same thing it does with an address, and for the same reason.
// The orientation decides the layout the generated mirror reproduces, and whether those sixteen floats are read
// as rows or as columns, which is what `mul` does with them.
// Left to the default it would come from a `#pragma pack_matrix` or a `-Zpr` set somewhere this file cannot see,
// and such a flag would transpose the cube with the mirror's size still correct.
struct cube_constants
{
    float4x4 view_projection;
};

#pragma sc push_constants
ConstantBuffer<cube_constants> gConstants;

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
