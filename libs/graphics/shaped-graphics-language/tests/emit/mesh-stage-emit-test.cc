#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

// The mesh stage as HLSL (EMIT-205), and the two targets that refuse it (EMIT-109, EMIT-206).

namespace
{
constexpr auto k_program
    = cc::string_view("require mesh_shader\n"
                      "struct corner:\n    @position position: hpos4\n    color: float4\n"
                      "@mesh(2, 2, vertices = 4, triangles = 2)\n"
                      "fun ms(@workgroup_id g: int3, @local_thread_index k: int, output: mut "
                      "mesh_output[corner]):\n"
                      "    output.set_counts(4u, 2u)\n"
                      "    output.vertex(k as uint, {position = hpos4(g.x as float, 0.0, 0.0, 1.0), "
                      "color = float4(1.0)})\n"
                      "    if k == 0:\n"
                      "        output.triangle(0u, uint3(0u, 1u, 2u))\n"
                      "        output.triangle(1u, uint3(0u, 2u, 3u))\n");
} // namespace

TEST("sgl emit - HLSL writes a mesh stage's output as its two arrays, and its calls as writes to them (EMIT-205)")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const e = emit_source(k_program, 0, t);
        REQUIRE(sgl::emit::dump_errors(e) == "");
        auto const& text = e.text;
        CHECK(text.contains("[outputtopology(\"triangle\")]\n[numthreads(2, 2, 1)]\n")).dump("text", text);
        CHECK(text.contains("out vertices corner output_vertices[4], out indices uint3 output_triangles[2])"))
            .dump("text", text);
        CHECK(text.contains("SV_GroupID")).dump("text", text);
        CHECK(text.contains("SetMeshOutputCounts(4u, 2u);")).dump("text", text);
        CHECK(text.contains("output_triangles[1u] = uint3(0u, 2u, 3u);")).dump("text", text);
        CHECK(text.contains("output_vertices[")).dump("text", text);
    }
}

TEST("sgl emit - WGSL and MSL refuse a mesh stage (EMIT-109, EMIT-206)")
{
    auto const wgsl = sgl::emit::dump_errors(emit_source(k_program, 0, target::wgsl));
    CHECK(wgsl.contains("needs mesh_shader, which WebGPU does not have")).dump("errors", wgsl);
    auto const msl = sgl::emit::dump_errors(emit_source(k_program, 0, target::msl));
    CHECK(msl.contains("is a mesh stage and needs mesh_shader")).dump("errors", msl);
}
