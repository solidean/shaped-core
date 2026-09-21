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
    // WebGPU reports the id unsigned and SGL has one integer type, so the conversion stands at the top.
    CHECK(wgsl.contains("    let id: vec3i = vec3i(id_in);\n"));
    CHECK(wgsl.contains("    work_values[id.x] = work_values[id.x] * 2.0;\n"));

    auto const hlsl = text_of(k_double, target::hlsl_dx12);
    CHECK(hlsl.contains("[numthreads(64, 1, 1)]\n"));
    CHECK(hlsl.contains("void double_values(uint3 id_in : SV_DispatchThreadID)\n"));
    CHECK(hlsl.contains("    const int3 id = int3(id_in);\n"));
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
