#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

namespace
{
cc::string emit_cube(isize index, target t)
{
    auto const e = emit_source(read_text(cc::string(SGL_SAMPLES_DIR) + "/cube.sgl"), index, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

// The six texts below are the twins of examples/graphics/rotating-cube/shaders/cube.hlsl, cube_vs.wgsl and cube_fs.wgsl.
// Each is pinned whole: an emitter's output is read by people, so a changed blank line is a change worth seeing.

TEST("sgl emit - the cube's vertex stage as HLSL for dx12")
{
    CHECK(emit_cube(0, target::hlsl_dx12)
          == "// SGL vertex entry point 'main_vs', written as HLSL for dx12.\n"
             "// Generated: the SGL source is what to edit.\n"
             "\n"
             "struct cube_vertex\n"
             "{\n"
             "    float3 position : POSITION;\n"
             "    float3 normal : NORMAL;\n"
             "    float3 color : COLOR;\n"
             "};\n"
             "\n"
             "struct pixel_input\n"
             "{\n"
             "    float4 position : SV_Position;\n"
             "    float3 normal : SGL0;\n"
             "    float3 color : SGL1;\n"
             "};\n"
             "\n"
             "struct constants_data\n"
             "{\n"
             "    column_major float4x4 view_projection;\n"
             "};\n"
             "\n"
             "ConstantBuffer<constants_data> constants : register(b0, space9);\n"
             "\n"
             "pixel_input main_vs(cube_vertex v)\n"
             "{\n"
             "    pixel_input result;\n"
             "    result.position = mul(constants.view_projection, float4(v.position, 1.0));\n"
             "    result.normal = v.normal;\n"
             "    result.color = v.color;\n"
             "    return result;\n"
             "}\n");
}

TEST("sgl emit - the cube's vertex stage as HLSL for vulkan")
{
    CHECK(emit_cube(0, target::hlsl_vulkan)
          == "// SGL vertex entry point 'main_vs', written as HLSL for vulkan.\n"
             "// Generated: the SGL source is what to edit.\n"
             "\n"
             "struct cube_vertex\n"
             "{\n"
             "    [[vk::location(0)]] float3 position : POSITION;\n"
             "    [[vk::location(1)]] float3 normal : NORMAL;\n"
             "    [[vk::location(2)]] float3 color : COLOR;\n"
             "};\n"
             "\n"
             "struct pixel_input\n"
             "{\n"
             "    float4 position : SV_Position;\n"
             "    [[vk::location(0)]] float3 normal : SGL0;\n"
             "    [[vk::location(1)]] float3 color : SGL1;\n"
             "};\n"
             "\n"
             "struct constants_data\n"
             "{\n"
             "    [[vk::offset(0)]] column_major float4x4 view_projection;\n"
             "};\n"
             "\n"
             "[[vk::push_constant]] ConstantBuffer<constants_data> constants;\n"
             "\n"
             "pixel_input main_vs(cube_vertex v)\n"
             "{\n"
             "    pixel_input result;\n"
             "    result.position = mul(constants.view_projection, float4(v.position, 1.0));\n"
             "    result.normal = v.normal;\n"
             "    result.color = v.color;\n"
             "    return result;\n"
             "}\n");
}

TEST("sgl emit - the cube's vertex stage as WGSL")
{
    CHECK(emit_cube(0, target::wgsl)
          == "// SGL vertex entry point 'main_vs', written as WGSL.\n"
             "// Generated: the SGL source is what to edit.\n"
             "\n"
             "struct cube_vertex {\n"
             "    @location(0) position: vec3f,\n"
             "    @location(1) normal: vec3f,\n"
             "    @location(2) color: vec3f,\n"
             "}\n"
             "\n"
             "struct pixel_input {\n"
             "    @builtin(position) position: vec4f,\n"
             "    @location(0) normal: vec3f,\n"
             "    @location(1) color: vec3f,\n"
             "}\n"
             "\n"
             "struct constants_data {\n"
             "    view_projection: mat4x4f,\n"
             "}\n"
             "\n"
             "@group(3) @binding(0) var<uniform> constants: constants_data;\n"
             "\n"
             "@vertex\n"
             "fn main_vs(v: cube_vertex) -> pixel_input {\n"
             "    return pixel_input(\n"
             "        constants.view_projection * vec4f(v.position, 1.0),\n"
             "        v.normal,\n"
             "        v.color\n"
             "    );\n"
             "}\n");
}

TEST("sgl emit - the cube's pixel stage as HLSL for dx12")
{
    CHECK(emit_cube(1, target::hlsl_dx12)
          == "// SGL pixel entry point 'main_ps', written as HLSL for dx12.\n"
             "// Generated: the SGL source is what to edit.\n"
             "\n"
             "struct pixel_input\n"
             "{\n"
             "    float4 position : SV_Position;\n"
             "    float3 normal : SGL0;\n"
             "    float3 color : SGL1;\n"
             "};\n"
             "\n"
             "struct target\n"
             "{\n"
             "    float4 color : SV_Target0;\n"
             "};\n"
             "\n"
             "target main_ps(pixel_input p)\n"
             "{\n"
             "    const float3 n = normalize(p.normal);\n"
             "    const float key = saturate(dot(n, normalize(float3(0.45, 0.8, -0.4))));\n"
             "    const float fill = saturate(dot(n, normalize(float3(-0.7, 0.15, 0.6))));\n"
             "    const float3 lit = p.color * (0.25 + 0.8 * key + 0.25 * fill);\n"
             "    target result;\n"
             "    result.color = float4(lit.x, lit.y, lit.z, 1.0);\n"
             "    return result;\n"
             "}\n");
}

TEST("sgl emit - the cube's pixel stage as HLSL for vulkan")
{
    CHECK(emit_cube(1, target::hlsl_vulkan)
          == "// SGL pixel entry point 'main_ps', written as HLSL for vulkan.\n"
             "// Generated: the SGL source is what to edit.\n"
             "\n"
             "struct pixel_input\n"
             "{\n"
             "    float4 position : SV_Position;\n"
             "    [[vk::location(0)]] float3 normal : SGL0;\n"
             "    [[vk::location(1)]] float3 color : SGL1;\n"
             "};\n"
             "\n"
             "struct target\n"
             "{\n"
             "    float4 color : SV_Target0;\n"
             "};\n"
             "\n"
             "target main_ps(pixel_input p)\n"
             "{\n"
             "    const float3 n = normalize(p.normal);\n"
             "    const float key = saturate(dot(n, normalize(float3(0.45, 0.8, -0.4))));\n"
             "    const float fill = saturate(dot(n, normalize(float3(-0.7, 0.15, 0.6))));\n"
             "    const float3 lit = p.color * (0.25 + 0.8 * key + 0.25 * fill);\n"
             "    target result;\n"
             "    result.color = float4(lit.x, lit.y, lit.z, 1.0);\n"
             "    return result;\n"
             "}\n");
}

TEST("sgl emit - the cube's pixel stage as WGSL, where `target` is reserved")
{
    CHECK(emit_cube(1, target::wgsl)
          == "// SGL pixel entry point 'main_ps', written as WGSL.\n"
             "// Generated: the SGL source is what to edit.\n"
             "\n"
             "struct pixel_input {\n"
             "    @builtin(position) position: vec4f,\n"
             "    @location(0) normal: vec3f,\n"
             "    @location(1) color: vec3f,\n"
             "}\n"
             "\n"
             "struct target_ {\n"
             "    @location(0) color: vec4f,\n"
             "}\n"
             "\n"
             "@fragment\n"
             "fn main_ps(p: pixel_input) -> target_ {\n"
             "    let n: vec3f = normalize(p.normal);\n"
             "    let key: f32 = saturate(dot(n, normalize(vec3f(0.45, 0.8, -0.4))));\n"
             "    let fill: f32 = saturate(dot(n, normalize(vec3f(-0.7, 0.15, 0.6))));\n"
             "    let lit: vec3f = p.color * (0.25 + 0.8 * key + 0.25 * fill);\n"
             "    return target_(vec4f(lit.x, lit.y, lit.z, 1.0));\n"
             "}\n");
}

TEST("sgl emit - emitting is deterministic, and a stage's text never names the other stage")
{
    auto const checked = check_sources(read_prelude(), read_text(cc::string(SGL_SAMPLES_DIR) + "/cube.sgl"));
    for (auto const t : sgl::emit::all_targets())
        for (auto index = isize(0); index < 2; ++index)
        {
            auto const first = sgl::emit::emit(checked.module, index, t);
            CHECK(first.has_text());
            CHECK(first == sgl::emit::emit(checked.module, index, t));
        }

    for (auto const t : sgl::emit::all_targets())
    {
        auto const vs = sgl::emit::emit(checked.module, 0, t).text;
        auto const ps = sgl::emit::emit(checked.module, 1, t).text;
        CHECK(!vs.contains("main_ps"));
        CHECK(!vs.contains("SV_Target"));
        CHECK(!ps.contains("main_vs"));
        CHECK(!ps.contains("cube_vertex"));
        CHECK(!ps.contains("constants"));
    }
}
