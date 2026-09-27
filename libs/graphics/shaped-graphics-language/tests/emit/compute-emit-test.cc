#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

namespace
{
/// The fixture SGL exists to make portable: double every element of a buffer, one thread per element.
constexpr cc::string_view k_double = "binding work:\n"
                                     "    values: mut buffer[float]\n"
                                     "\n"
                                     "@compute(64) fun double_values(@thread_id id: int3){work}:\n"
                                     "    work.values[id.x] = work.values[id.x] * 2.0\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

TEST("sgl emit - a compute entry point carries its workgroup size")
{
    auto const wgsl = text_of(k_double, target::wgsl);
    CHECK(wgsl.contains("@compute @workgroup_size(64, 1, 1)\n"));
    CHECK(wgsl.contains("fn double_values(@builtin(global_invocation_id) id_in: vec3u) {\n"));
    // WebGPU reports the id unsigned and SGL's is an int3, so the conversion stands at the top.
    CHECK(wgsl.contains("    let id: vec3i = vec3i(id_in);\n"));
    CHECK(wgsl.contains("    work_values[id.x] = work_values[id.x] * 2.0;\n"));

    auto const hlsl = text_of(k_double, target::hlsl_dx12);
    CHECK(hlsl.contains("[numthreads(64, 1, 1)]\n"));
    CHECK(hlsl.contains("void double_values(uint3 id_in : SV_DispatchThreadID)\n"));
    CHECK(hlsl.contains("    const int3 id = int3(id_in);\n"));
}

TEST("sgl emit - `as` is a conversion to the target's own type")
{
    constexpr auto converted = "binding work:\n"
                               "    values: mut buffer[float]\n"
                               "\n"
                               "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                               "    let uv = int2(id.x, id.y) as float2\n"
                               "    let u = id.x as uint\n"
                               "    work.values[id.x] = uv.y + (u as float)\n";
    auto const wgsl = text_of(converted, target::wgsl);
    CHECK(wgsl.contains("    let uv: vec2f = vec2f(vec2i(id.x, id.y));\n"));
    CHECK(wgsl.contains("    let u: u32 = u32(id.x);\n"));
    CHECK(wgsl.contains("uv.y + f32(u);\n"));

    auto const hlsl = text_of(converted, target::hlsl_dx12);
    CHECK(hlsl.contains("    const float2 uv = float2(int2(id.x, id.y));\n"));
    CHECK(hlsl.contains("    const uint u = uint(id.x);\n"));
    CHECK(hlsl.contains("uv.y + float(u);\n"));
}

TEST("sgl emit - every axis of the workgroup reaches the text")
{
    constexpr auto tiled = "binding work:\n"
                           "    values: mut buffer[float]\n"
                           "\n"
                           "@compute(8, 4, 2) fun tile(@thread_id id: int3){work}:\n"
                           "    work.values[id.x + id.y + id.z] = 1.0\n";
    CHECK(text_of(tiled, target::wgsl).contains("@compute @workgroup_size(8, 4, 2)\n"));
    CHECK(text_of(tiled, target::hlsl_dx12).contains("[numthreads(8, 4, 2)]\n"));
}

TEST("sgl emit - the dispatch id's unsigned twin is minted, so a local of the program cannot take its name")
{
    auto const wgsl = text_of("binding work:\n"
                              "    values: mut buffer[float]\n"
                              "\n"
                              "@compute(64) fun main(@thread_id id: int3){work}:\n"
                              "    let id_in = 2.0\n"
                              "    work.values[id.x] = id_in\n",
                              target::wgsl);
    CHECK(wgsl.contains("fn main(@builtin(global_invocation_id) id_in_1: vec3u) {\n"));
    CHECK(wgsl.contains("    let id: vec3i = vec3i(id_in_1);\n"));
    CHECK(wgsl.contains("    let id_in: f32 = 2.0;\n"));
}

TEST("sgl emit - MSL refuses a compute entry point, which it writes as a kernel")
{
    // EMIT-13's exception: the three other targets write it.
    constexpr auto plain = "@compute(64) fun main(@thread_id id: int3):\n"
                           "    let x = id.x\n";
    CHECK(sgl::emit::dump_errors(emit_source(plain, 0, target::msl))
          == "unsupported a compute entry point, which MSL writes as a kernel\n");
    CHECK(sgl::emit::dump_errors(emit_source(plain, 0, target::wgsl)) == "");
}

TEST("sgl emit - a compute entry point that needs legalizing keeps its workgroup and its thread id")
{
    // The early return makes the tree structured, so legalizing rewrites it; the entry point's own facts must survive.
    constexpr auto clamped = "binding work:\n"
                             "    values: mut buffer[float]\n"
                             "\n"
                             "fun clamp_to(x: float, hi: float) -> float:\n"
                             "    if x > hi => return hi\n"
                             "    return x\n"
                             "\n"
                             "@compute(8, 4, 2) fun main(@thread_id id: int3){work}:\n"
                             "    work.values[id.x] = clamp_to(work.values[id.x], 4.0)\n";
    auto const wgsl = text_of(clamped, target::wgsl);
    CHECK(wgsl.contains("@compute @workgroup_size(8, 4, 2)\n"));
    CHECK(wgsl.contains("    let id: vec3i = vec3i(id_in);\n"));
    CHECK(wgsl.contains("    work_values[id.x] = clamp_to_result;\n"));
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const hlsl = text_of(clamped, t);
        CHECK(hlsl.contains("[numthreads(8, 4, 2)]\n"));
        CHECK(hlsl.contains("    const int3 id = int3(id_in);\n"));
    }
}

TEST("sgl emit - every id a dispatch hands over is a parameter of its own, converted where the body reads it")
{
    constexpr auto ids
        = "binding work:\n"
          "    values: mut buffer[int]\n"
          "\n"
          "@compute(8, 8) fun ids(@thread_id id: int3, @local_thread_id l: int3, @local_thread_index li: int, "
          "@workgroup_id g: int3){work}:\n"
          "    work.values[id.x] = li + l.y + g.x\n";
    auto const hlsl = text_of(ids, target::hlsl_dx12);
    CHECK(hlsl.contains("void ids(uint3 id_in : SV_DispatchThreadID, uint3 l_in : SV_GroupThreadID, uint li_in : "
                        "SV_GroupIndex, uint3 g_in : SV_GroupID)\n"));
    CHECK(hlsl.contains("    const int li = int(li_in);\n"));
    auto const wgsl = text_of(ids, target::wgsl);
    CHECK(wgsl.contains("fn ids(@builtin(global_invocation_id) id_in: vec3u, @builtin(local_invocation_id) l_in: "
                        "vec3u, "
                        "@builtin(local_invocation_index) li_in: u32, @builtin(workgroup_id) g_in: vec3u) {\n"));
    CHECK(wgsl.contains("    let g: vec3i = vec3i(g_in);\n"));
}

TEST("sgl emit - HLSL counts a vertex and an instance from the draw's base, so the text adds it back")
{
    // EMIT-114: every other target counts from the draw's first vertex and instance, which is SGL's meaning
    constexpr auto source = "struct varyings:\n"
                            "    @position position: hpos4\n"
                            "\n"
                            "@vertex fun vs(@vertex_index v: int, @instance_index i: int) -> varyings:\n"
                            "    return { position = hpos4(v as float, i as float, 0.0, 1.0) }\n";
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const hlsl = text_of(source, t);
        CHECK(hlsl.contains("varyings vs(uint v_in : SV_VertexID, uint v_base : SV_StartVertexLocation, uint i_in : "
                            "SV_InstanceID, uint i_base : SV_StartInstanceLocation)\n"));
        CHECK(hlsl.contains("    const int v = int(v_in + v_base);\n"));
        CHECK(hlsl.contains("    const int i = int(i_in + i_base);\n"));
    }
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("fn vs(@builtin(vertex_index) v_in: u32, @builtin(instance_index) i_in: u32) -> varyings {\n"));
    CHECK(wgsl.contains("    let v: i32 = i32(v_in);\n"));
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("vertex varyings vs(uint v_in [[vertex_id]], uint i_in [[instance_id]])\n"));
}

TEST("sgl emit - WGSL enables primitive_index before it reads one")
{
    constexpr auto source = "require primitive_index\n"
                            "struct varyings:\n"
                            "    @position position: hpos4\n"
                            "\n"
                            "@pixel struct target:\n"
                            "    color: float4\n"
                            "\n"
                            "@pixel fun ps(p: varyings, @primitive_id id: int) -> target:\n"
                            "    return { color = float4(id as float, 0.0, 0.0, 1.0) }\n";
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.starts_with("// SGL pixel entry point 'ps', written as WGSL.\n// Generated: the SGL source is what to "
                           "edit.\n\nenable primitive_index;\n"));
    CHECK(wgsl.contains("@builtin(primitive_index) id_in: u32"));
    CHECK(text_of(source, target::hlsl_dx12).contains("uint id_in : SV_PrimitiveID"));
    CHECK(text_of(source, target::msl).contains("uint id_in [[primitive_id]]"));
}
