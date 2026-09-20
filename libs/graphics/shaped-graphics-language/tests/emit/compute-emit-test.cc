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
