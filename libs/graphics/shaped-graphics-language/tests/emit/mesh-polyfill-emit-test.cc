#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::mesh_form;
using sgl::emit::target;

// A mesh stage polyfilled as a compute entry point that writes its workgroups' slots, and a vertex entry point that
// draws them back (CHK-524).

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

TEST("sgl emit - a polyfilled mesh stage's two halves agree on its workgroup's slots (CHK-524)")
{
    for (auto const t : {target::wgsl, target::hlsl_dx12, target::hlsl_vulkan})
        for (auto const form : {mesh_form::compute, mesh_form::vertex_fetch})
        {
            auto const e = emit_source(k_program, 0, t, {.mesh = form});
            REQUIRE(sgl::emit::dump_errors(e) == "");
            REQUIRE(e.mesh_layout.has_value());
            auto const& l = e.mesh_layout.value();
            CHECK(l.corners == 3);
            CHECK(l.max_vertices == 4);
            CHECK(l.max_primitives == 2);
            // a hpos4 and a float4, one word a lane
            CHECK(l.vertex_words == 8);
            CHECK(l.group_words == 2 + 3 * 2 + 4 * 8);
        }
}

TEST("sgl emit - a polyfilled mesh stage's compute half writes sg_mesh_output on WebGPU, which has no mesh stage")
{
    auto const e = emit_source(k_program, 0, target::wgsl, {.mesh = mesh_form::compute});
    REQUIRE(sgl::emit::dump_errors(e) == "");
    auto const& text = e.text;
    CHECK(text.contains("@compute @workgroup_size(2, 2, 1)")).dump("text", text);
    CHECK(text.contains("sg_mesh_output")).dump("text", text);
    CHECK(text.contains("@group(3) @binding(23)")).dump("text", text);
    CHECK(text.contains("sg_dispatch_grid")).dump("text", text);
    CHECK(!text.contains("set_counts")).dump("text", text);
}

TEST("sgl emit - a polyfilled mesh stage's vertex half reads sg_mesh_input, and clips a slot past its count")
{
    auto const e = emit_source(k_program, 0, target::wgsl, {.mesh = mesh_form::vertex_fetch});
    REQUIRE(sgl::emit::dump_errors(e) == "");
    auto const& text = e.text;
    CHECK(text.contains("@vertex")).dump("text", text);
    CHECK(text.contains("sg_mesh_input")).dump("text", text);
    CHECK(text.contains("@group(3) @binding(24)")).dump("text", text);
    CHECK(text.contains("@builtin(vertex_index)")).dump("text", text);
    CHECK(text.contains("2.0")).dump("text", text);

    auto const hlsl = emit_source(k_program, 0, target::hlsl_dx12, {.mesh = mesh_form::vertex_fetch});
    REQUIRE(sgl::emit::dump_errors(hlsl) == "");
    CHECK(hlsl.text.contains("SV_VertexID")).dump("text", hlsl.text);
    CHECK(hlsl.text.contains("space11")).dump("text", hlsl.text);
}

TEST("sgl emit - a mesh stage the polyfill does not carry yet is refused by name, not guessed (CHK-524)")
{
    constexpr auto per_primitive = cc::string_view("require mesh_shader\n"
                                                   "struct corner:\n    @position position: hpos4\n"
                                                   "struct face:\n    tint: float4\n"
                                                   "@mesh(1, vertices = 3, triangles = 1)\n"
                                                   "fun ms(output: mut mesh_output[corner, face]):\n"
                                                   "    output.set_counts(0u, 0u)\n");
    auto const faces = sgl::emit::dump_errors(emit_source(per_primitive, 0, target::wgsl, {.mesh = mesh_form::compute}));
    CHECK(faces.contains("writes values per primitive")).dump("errors", faces);

    constexpr auto with_task = cc::string_view("require mesh_shader\n"
                                               "struct corner:\n    @position position: hpos4\n"
                                               "struct handed:\n    blue: float\n"
                                               "@task(1) fun ts():\n"
                                               "    let h: handed = {blue = 1.0}\n"
                                               "    dispatch_mesh(1u, 1u, 1u, h)\n"
                                               "@mesh(1, vertices = 3, triangles = 1)\n"
                                               "fun ms(h: handed, output: mut mesh_output[corner]):\n"
                                               "    output.set_counts(0u, 0u)\n");
    auto const task = sgl::emit::dump_errors(emit_source(with_task, 1, target::wgsl, {.mesh = mesh_form::compute}));
    CHECK(task.contains("payload")).dump("errors", task);
}
