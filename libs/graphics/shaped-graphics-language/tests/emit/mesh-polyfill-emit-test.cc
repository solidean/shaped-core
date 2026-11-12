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

TEST("sgl emit - a polyfilled mesh stage that reads no stage input still numbers its workgroup (CHK-524)")
{
    constexpr auto program = cc::string_view("require mesh_shader\n"
                                             "struct corner:\n    @position position: hpos4\n"
                                             "@mesh(1, vertices = 3, triangles = 1)\n"
                                             "fun ms(output: mut mesh_output[corner]):\n"
                                             "    output.set_counts(3u, 1u)\n"
                                             "    output.vertex(0u, {position = hpos4(0.0, 0.0, 0.0, 1.0)})\n"
                                             "    output.triangle(0u, uint3(0u, 0u, 0u))\n");
    for (auto const form : {mesh_form::compute, mesh_form::vertex_fetch})
    {
        auto const e = emit_source(program, 0, target::wgsl, {.mesh = form});
        REQUIRE(sgl::emit::dump_errors(e) == "");
        if (form == mesh_form::compute)
            CHECK(e.text.contains("@builtin(workgroup_id)")).dump("text", e.text);
    }
}

TEST("sgl emit - a polyfilled mesh stage's values per primitive follow its vertices, and ride flat on each corner")
{
    // `face.color` shares a name with the vertex's member, which the struct both halves hand on must not
    constexpr auto program
        = cc::string_view("require mesh_shader\n"
                          "struct corner:\n    @position position: hpos4\n    color: float4\n"
                          "struct face:\n    color: float2\n    id: uint\n"
                          "@pixel struct painted:\n    color: float4\n"
                          "@mesh(1, vertices = 3, triangles = 2)\n"
                          "fun ms(output: mut mesh_output[corner, face]):\n"
                          "    output.set_counts(0u, 0u)\n"
                          "@pixel fun ps(c: corner, f: face) -> painted:\n"
                          "    return {color = c.color + float4(f.color.x, f.color.y, f.id as float, 0.0)}\n");
    for (auto const form : {mesh_form::compute, mesh_form::vertex_fetch})
    {
        auto const e = emit_source(program, 0, target::wgsl, {.mesh = form});
        REQUIRE(sgl::emit::dump_errors(e) == "");
        auto const& l = e.mesh_layout.value();
        CHECK(l.primitive_words == 3);
        CHECK(l.first_primitive_word() == 2 + 3 * 2 + 3 * 8);
        CHECK(l.group_words == l.first_primitive_word() + 2 * 3);
    }

    auto const vertex = emit_source(program, 0, target::wgsl, {.mesh = mesh_form::vertex_fetch});
    CHECK(vertex.text.contains("@location(1) @interpolate(flat) color_of_primitive: vec2f")).dump("text", vertex.text);
    CHECK(vertex.text.contains("@location(2) @interpolate(flat) id: u32")).dump("text", vertex.text);

    // the pixel stage after the vertex half takes the same struct, and makes its own two from it
    for (auto const t : {target::wgsl, target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const pixel = emit_source(program, 1, t, {.mesh = mesh_form::vertex_fetch});
        REQUIRE(sgl::emit::dump_errors(pixel) == "");
        CHECK(pixel.text.contains("sgl_mesh_corner")).dump("text", pixel.text);
        CHECK(!pixel.text.contains("5271")).dump("text", pixel.text);
    }

    // EMIT-261: as itself, only a native mesh stage feeds it, which WebGPU has none of
    auto const refused = sgl::emit::dump_errors(emit_source(program, 1, target::wgsl, {}));
    CHECK(refused.contains("target-lacks-feature")).dump("errors", refused);
    CHECK(refused.contains("--mesh vertex")).dump("errors", refused);
    CHECK(sgl::emit::dump_errors(emit_source(program, 1, target::hlsl_vulkan, {})) == "");
}

TEST("sgl emit - a polyfilled task stage writes its first invocation's launch into its record (CHK-524)")
{
    constexpr auto program = cc::string_view("require mesh_shader\n"
                                             "struct handed:\n    blue: float\n    id: uint3\n"
                                             "@task(8, meshes = 3) fun ts():\n"
                                             "    let h: handed = {blue = 1.0, id = uint3(1u)}\n"
                                             "    dispatch_mesh(5u, 1u, 1u, h)\n");
    for (auto const t : {target::wgsl, target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const e = emit_source(program, 0, t, {.mesh = mesh_form::compute});
        REQUIRE(sgl::emit::dump_errors(e) == "");
        REQUIRE(e.task_layout.has_value());
        // a float and a uint3 behind the launch's grid and its first
        CHECK(e.task_layout.value().payload_words == 4);
        CHECK(e.task_layout.value().record_words() == 8);
        CHECK(e.task_layout.value().max_meshes == 3);
        CHECK(e.text.contains("sg_task_output")).dump("text", e.text);
        CHECK(!e.text.contains("DispatchMesh")).dump("text", e.text);
    }
    auto const wgsl = emit_source(program, 0, target::wgsl, {.mesh = mesh_form::compute});
    CHECK(wgsl.text.contains("@compute @workgroup_size(8, 1, 1)")).dump("text", wgsl.text);
    // a workgroup past the records the draw holds writes none
    CHECK(wgsl.text.contains("sg_dispatch_grid")).dump("text", wgsl.text);
}

TEST("sgl emit - an indirect draw of a polyfilled mesh stage launches its compute half by one record of no payload")
{
    for (auto const t : {target::wgsl, target::hlsl_dx12, target::hlsl_vulkan})
        for (auto const form : {mesh_form::launched, mesh_form::scan})
        {
            auto const e = emit_source(k_program, 0, t, {.mesh = form});
            REQUIRE(sgl::emit::dump_errors(e) == "");
            REQUIRE(e.task_layout.has_value());
            CHECK(e.task_layout.value().record_words() == 4);
            CHECK(e.task_layout.value().max_meshes == 0);
            CHECK(e.mesh_layout.value().group_words == 2 + 3 * 2 + 4 * 8);
        }
    auto const launched = emit_source(k_program, 0, target::wgsl, {.mesh = mesh_form::launched});
    CHECK(launched.text.contains("sg_task_input")).dump("text", launched.text);
    CHECK(launched.text.contains("sg_mesh_output")).dump("text", launched.text);
    auto const scan = emit_source(k_program, 0, target::wgsl, {.mesh = mesh_form::scan});
    CHECK(scan.text.contains("@workgroup_size(1, 1, 1)")).dump("text", scan.text);
    CHECK(scan.text.contains("sg_task_output")).dump("text", scan.text);
}

TEST("sgl emit - a mesh stage the polyfill does not carry yet is refused by name, not guessed (CHK-524)")
{
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
