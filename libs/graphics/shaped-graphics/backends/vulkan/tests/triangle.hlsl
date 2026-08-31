// triangle.hlsl — minimal vertex + fragment pair for the vulkan raster end-to-end test.
//
// One oversized triangle covering the whole target, its color carried per vertex, so a readback can check both the
// clear (a scope that draws nothing) and the draw (a scope that covers every pixel).
//
// The `[[vk::location]]` annotations are load-bearing.
// sg identifies a vertex attribute by an HLSL semantic string and SPIR-V has none, so the vulkan backend numbers
// locations by an attribute's position in `vertex_input_layout::attributes` — these annotations are what the shader
// side of that agreement looks like.
// See vulkan_raster_pipeline.cc, where the workaround names the sg::vertex_attribute::location field that replaces it.
//
// Compiled to SPIR-V and embedded as triangle.spirv.h (compilation is not part of the build yet):
//   dxc -T vs_6_0 -E vs_main -spirv -fspv-target-env=vulkan1.3 -Fh triangle.vs.spirv.h -Vn triangle_vs_spirv triangle.hlsl
//   dxc -T ps_6_0 -E ps_main -spirv -fspv-target-env=vulkan1.3 -Fh triangle.ps.spirv.h -Vn triangle_ps_spirv triangle.hlsl
//   dxc -T ps_6_0 -E ps_from_buffer -spirv -fspv-target-env=vulkan1.3 -Fh triangle.psbuf.spirv.h -Vn triangle_psbuf_spirv triangle.hlsl

struct vs_in
{
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR;
};

struct vs_out
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

vs_out vs_main(vs_in v)
{
    vs_out o;
    o.position = float4(v.position, 0.0f, 1.0f);
    o.color = v.color;
    return o;
}

float4 ps_main(vs_out i) : SV_Target
{
    return i.color;
}

// A second fragment stage that reads a bound buffer, so a draw can depend on a compute dispatch in the same list.
// That is what forces a barrier while a rendering scope is open — the case Vulkan forbids outright and the backend
// answers by closing and reopening the instance around it.
[[vk::binding(0, 0)]] StructuredBuffer<uint> Values : register(t0);

float4 ps_from_buffer(vs_out i) : SV_Target
{
    return float4(float(Values[3]) / 255.0f, 0.0f, 0.0f, 1.0f);
}
