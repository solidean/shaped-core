#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

// The mesh stage as HLSL (EMIT-205) and as Metal's mesh object (EMIT-206), and WGSL, which refuses it (EMIT-109).

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

TEST("sgl emit - WGSL refuses a mesh stage, and MSL writes it as Metal's mesh object (EMIT-109, EMIT-206)")
{
    auto const wgsl = sgl::emit::dump_errors(emit_source(k_program, 0, target::wgsl));
    CHECK(wgsl.contains("needs mesh_shader, which WebGPU does not have")).dump("errors", wgsl);
    auto const e = emit_source(k_program, 0, target::msl);
    REQUIRE(sgl::emit::dump_errors(e) == "");
    auto const& msl = e.text;
    CHECK(msl.contains("[[mesh]] void ms(")).dump("text", msl);
    CHECK(msl.contains("metal::mesh<corner, void, 4, 2, metal::topology::triangle> output")).dump("text", msl);
    // Metal counts the primitives alone, and a triangle is three indices
    CHECK(msl.contains("output.set_primitive_count(2u);")).dump("text", msl);
    CHECK(msl.contains("output.set_vertex(")).dump("text", msl);
    CHECK(msl.contains(".set_index(")).dump("text", msl);
}

TEST("sgl emit - HLSL hands a task stage's payload over from groupshared memory, and a mesh stage takes it in "
     "(EMIT-207, "
     "EMIT-208)")
{
    constexpr auto program = cc::string_view("require mesh_shader\n"
                                             "struct corner:\n    @position position: hpos4\n"
                                             "struct handed:\n    blue: float\n"
                                             "@task(8) fun ts():\n"
                                             "    let h: handed = {blue = 1.0}\n"
                                             "    dispatch_mesh(2u, 1u, 1u, h)\n"
                                             "@mesh(1, vertices = 3, triangles = 1)\n"
                                             "fun ms(h: handed, output: mut mesh_output[corner]):\n"
                                             "    output.set_counts(0u, 0u)\n");
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const task = emit_source(program, 0, t);
        REQUIRE(sgl::emit::dump_errors(task) == "");
        CHECK(task.text.contains("groupshared handed sgl_payload;")).dump("text", task.text);
        CHECK(task.text.contains("[numthreads(8, 1, 1)]")).dump("text", task.text);
        CHECK(task.text.contains("sgl_payload = h;")).dump("text", task.text);
        CHECK(task.text.contains("DispatchMesh(2u, 1u, 1u, sgl_payload);")).dump("text", task.text);
        auto const mesh = emit_source(program, 1, t);
        REQUIRE(sgl::emit::dump_errors(mesh) == "");
        CHECK(mesh.text.contains("in payload handed h,")).dump("text", mesh.text);
    }
    CHECK(sgl::emit::dump_errors(emit_source(program, 0, target::wgsl)).contains("WebGPU does not have"));
    // Metal's object stage writes its payload and its grid from its first invocation, and the mesh stage takes it
    auto const task = emit_source(program, 0, target::msl);
    REQUIRE(sgl::emit::dump_errors(task) == "");
    CHECK(task.text.contains("[[object]] void ts(object_data handed& sgl_payload [[payload]], mesh_grid_properties "
                             "sgl_grid, uint sgl_thread [[thread_index_in_threadgroup]])"))
        .dump("text", task.text);
    CHECK(task.text.contains("sgl_grid.set_threadgroups_per_grid(uint3(2u, 1u, 1u));")).dump("text", task.text);
    auto const mesh = emit_source(program, 1, target::msl);
    REQUIRE(sgl::emit::dump_errors(mesh) == "");
    CHECK(mesh.text.contains("const object_data handed& h [[payload]]")).dump("text", mesh.text);
}

TEST("sgl emit - a per-triangle struct is out primitives in a mesh stage, and a flat per-primitive input in the pixel "
     "stage (EMIT-209)")
{
    constexpr auto program
        = cc::string_view("require mesh_shader\n"
                          "struct corner:\n    @position position: hpos4\n    shade: float4\n"
                          "struct face:\n    tint: float4\n"
                          "@pixel struct painted:\n    color: float4\n"
                          "@mesh(1, vertices = 3, triangles = 1)\n"
                          "fun ms(output: mut mesh_output[corner, face]):\n"
                          "    output.set_counts(3u, 1u)\n"
                          "    output.primitive(0u, {tint = float4(1.0)})\n"
                          "@pixel fun ps(c: corner, f: face) -> painted => { color = c.shade + f.tint }\n");
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const mesh = emit_source(program, 0, t);
        REQUIRE(sgl::emit::dump_errors(mesh) == "");
        CHECK(mesh.text.contains("out primitives face output_primitives[1])")).dump("text", mesh.text);
        CHECK(mesh.text.contains("output_primitives[0u] = ")).dump("text", mesh.text);
        // its locations follow the stage link's, in the space the pixel stage reads both from
        CHECK(mesh.text.contains("float4 tint : SGL1;")).dump("text", mesh.text);
        auto const pixel = emit_source(program, 1, t);
        REQUIRE(sgl::emit::dump_errors(pixel) == "");
        CHECK(pixel.text.contains("(corner c, face f)")).dump("text", pixel.text);
        CHECK(pixel.text.contains("nointerpolation float4 tint : SGL1;")).dump("text", pixel.text);
        CHECK(pixel.text.contains("[[vk::ext_decorate(5271)]]") == (t == target::hlsl_vulkan)).dump("text", pixel.text);
        CHECK(pixel.text.contains("[[vk::ext_capability(5283)]]") == (t == target::hlsl_vulkan)).dump("text", pixel.text);
    }
}

TEST("sgl emit - HLSL writes a mesh stage of lines as a line topology over two indices each (EMIT-205)")
{
    constexpr auto lines = cc::string_view("require mesh_shader\n"
                                           "struct corner:\n    @position position: hpos4\n"
                                           "@mesh(1, vertices = 2, lines = 1)\n"
                                           "fun ms(output: mut mesh_output[corner]):\n"
                                           "    output.set_counts(2u, 1u)\n"
                                           "    output.vertex(0u, {position = hpos4(0.0, 0.0, 0.0, 1.0)})\n"
                                           "    output.vertex(1u, {position = hpos4(1.0, 0.0, 0.0, 1.0)})\n"
                                           "    output.line(0u, uint2(0u, 1u))\n");
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const e = emit_source(lines, 0, t);
        REQUIRE(sgl::emit::dump_errors(e) == "");
        CHECK(e.text.contains("[outputtopology(\"line\")]\n")).dump("text", e.text);
        CHECK(e.text.contains("out indices uint2 output_lines[1])")).dump("text", e.text);
        CHECK(e.text.contains("output_lines[0u] = uint2(0u, 1u);")).dump("text", e.text);
    }
}
