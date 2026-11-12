#include "check-test-support.hh"

using namespace sgl_test;

// The mesh stage, CHK-480 to CHK-483: each is a C++ test rather than a corpus entry, since WGSL and MSL refuse the
// stage and a corpus entry point is emitted on every target.

namespace
{
constexpr auto k_head = cc::string_view("require mesh_shader\n"
                                        "struct corner:\n    @position position: hpos4\n    color: float4\n"
                                        "@pixel struct target:\n    color: float4\n"
                                        "@pixel fun ps(c: corner) -> target => { color = c.color }\n");

constexpr auto k_mesh
    = cc::string_view("@mesh(4, vertices = 3, triangles = 1)\n"
                      "fun ms(@local_thread_index k: int, output: mut mesh_output[corner]):\n"
                      "    output.set_counts(3u, 1u)\n"
                      "    if k < 3:\n"
                      "        output.vertex(k as uint, {position = hpos4(0.0, 0.0, 0.0, 1.0), color = "
                      "float4(1.0)})\n"
                      "    if k == 0:\n"
                      "        output.triangle(0u, uint3(0u, 1u, 2u))\n");

cc::string with_head(cc::string_view program)
{
    return reports_for(cc::string(k_head) + program);
}
} // namespace

TEST("sgl check - a mesh stage and its pipeline check as they are (CHK-480 to CHK-483)")
{
    CHECK(with_head(cc::string(k_mesh) + "pipeline:\n    mesh = ms\n    pixel = ps\n    format = .rgba8_unorm\n") == "");
}

TEST("sgl check - @mesh takes a workgroup and the bounds of what it writes (CHK-480)")
{
    auto const body = cc::string_view(" fun ms(output: mut mesh_output[corner]):\n    output.set_counts(0u, 0u)\n");
    CHECK(with_head(cc::string("@mesh(32, vertices = 64, triangles = 126)") + body) == "");
    for (auto const attribute :
         {"@mesh(32)", "@mesh(vertices = 3, triangles = 1)", "@mesh(1, vertices = 300, triangles = 1)",
          "@mesh(1, vertices = 3, triangles = 1, vertices = 3)", "@mesh(1, 2, 3, 4, vertices = 3, triangles = 1)"})
        CHECK(with_head(cc::string(attribute) + body).contains("invalid-attribute-arguments"))
            .dump("attribute", cc::string(attribute));
    CHECK(with_head(cc::string("@mesh(16, 16, vertices = 3, triangles = 1)") + body)
              .contains("a mesh workgroup holds at most 128 invocations"));
}

TEST("sgl check - a mesh stage needs mesh_shader")
{
    auto const without = reports_for("struct corner:\n    @position position: hpos4\n"
                                     "@mesh(1, vertices = 3, triangles = 1) fun ms(output: mut mesh_output[corner]):\n"
                                     "    output.set_counts(0u, 0u)\n");
    CHECK(without.contains("feature-not-declared")).dump("reports", without);
    CHECK(without.contains("mesh_shader")).dump("reports", without);
}

TEST("sgl check - a mesh stage takes stage inputs and last its output, and returns nothing (CHK-481)")
{
    auto const no_output = with_head("@mesh(1, vertices = 3, triangles = 1) fun ms(@local_thread_index k: int):\n"
                                     "    let x = k\n");
    CHECK(no_output.contains("last its output")).dump("reports", no_output);
    auto const returns = with_head("@mesh(1, vertices = 3, triangles = 1)\n"
                                   "fun ms(output: mut mesh_output[corner]) -> int:\n    return 1\n");
    CHECK(returns.contains("returns nothing")).dump("reports", returns);
    auto const no_position = with_head("struct flat:\n    color: float4\n"
                                       "@mesh(1, vertices = 3, triangles = 1) fun ms(output: mut mesh_output[flat]):\n"
                                       "    output.set_counts(0u, 0u)\n");
    CHECK(no_position.contains("one @position field")).dump("reports", no_position);
    // a mesh output is no value, as a stream is none
    auto const as_value = with_head("fun helper(o: mesh_output[corner]) => 1\n");
    CHECK(!as_value.empty());
}

TEST("sgl check - a mesh output's vertex takes one of its own type at an index (CHK-482)")
{
    auto const wrong = with_head("struct other:\n    @position position: hpos4\n"
                                 "@mesh(1, vertices = 3, triangles = 1) fun ms(output: mut mesh_output[corner]):\n"
                                 "    output.set_counts(1u, 0u)\n"
                                 "    output.vertex(0u, {position = hpos4(0.0, 0.0, 0.0, 1.0)})\n");
    CHECK(!wrong.empty());
    auto const extra = with_head("@mesh(1, vertices = 3, triangles = 1) fun ms(output: mut mesh_output[corner]):\n"
                                 "    output.vertex(0u)\n");
    CHECK(extra.contains("vertex takes an index and the value written there")).dump("reports", extra);
}

TEST("sgl check - a mesh output's counts are set in control flow the whole workgroup takes (CHK-482)")
{
    auto const varying = with_head("@mesh(4, vertices = 3, triangles = 1)\n"
                                   "fun ms(@local_thread_index k: int, output: mut mesh_output[corner]):\n"
                                   "    if k == 0:\n"
                                   "        output.set_counts(3u, 1u)\n");
    CHECK(varying.contains("non-uniform-control-flow")).dump("reports", varying);
}

TEST("sgl check - a mesh stage takes the vertex stage's place in a pipeline (CHK-483)")
{
    auto const vertex
        = cc::string_view("@vertex fun vs(@vertex_index i: int) -> corner => { position = hpos4(0.0, 0.0, "
                          "0.0, 1.0), color = float4(1.0) }\n");
    auto const both
        = with_head(cc::string(k_mesh) + vertex
                    + "pipeline:\n    vertex = vs\n    mesh = ms\n    pixel = ps\n    format = .rgba8_unorm\n");
    CHECK(both.contains("a pipeline with a mesh stage has no vertex, tessellation or geometry stage")).dump("reports", both);
    // what the mesh stage writes is what the pixel stage takes
    auto const mismatched = with_head(cc::string(k_mesh) + "struct other:\n    @position position: hpos4\n"
                                      + "@pixel fun ps2(o: other) -> target => { color = float4(1.0) }\n"
                                      + "pipeline:\n    mesh = ms\n    pixel = ps2\n    format = .rgba8_unorm\n");
    CHECK(mismatched.contains("member for member")).dump("reports", mismatched);
}

TEST("sgl check - @workgroup memory and barriers belong to a mesh stage too")
{
    CHECK(with_head("@workgroup binding shared:\n    count: atomic[uint]\n"
                    "@mesh(32, vertices = 3, triangles = 1)\n"
                    "fun ms(@local_thread_index k: int, output: mut mesh_output[corner]){shared}:\n"
                    "    shared.count.add(1u)\n"
                    "    workgroup_barrier()\n"
                    "    output.set_counts(0u, 0u)\n")
          == "");
}

namespace
{
constexpr auto k_task = cc::string_view("struct handed:\n    blue: float\n"
                                        "@task(1) fun ts():\n"
                                        "    let h: handed = {blue = 1.0}\n"
                                        "    dispatch_mesh(2u, 1u, 1u, h)\n"
                                        "@mesh(1, vertices = 3, triangles = 1)\n"
                                        "fun ms(h: handed, output: mut mesh_output[corner]):\n"
                                        "    output.set_counts(0u, 0u)\n");
} // namespace

TEST("sgl check - a task stage and its mesh stage check as a pipeline (CHK-484 to CHK-487)")
{
    CHECK(with_head(cc::string(k_task)
                    + "pipeline:\n    task = ts\n    mesh = ms\n    pixel = ps\n    format = .rgba8_unorm\n")
          == "");
}

TEST("sgl check - @task may state how many mesh workgroups one launches at most (CHK-484)")
{
    auto const source = [](cc::string_view attribute)
    {
        return cc::string(k_head) + "struct handed:\n    blue: float\n" + attribute
             + " fun ts():\n    let h: handed = {blue = 1.0}\n    dispatch_mesh(2u, 1u, 1u, h)\n";
    };
    auto const checked = check_sources(read_prelude(), source("@task(32, meshes = 64)"));
    CHECK(reports_of(checked) == "");
    auto const& ts = symbol_named(checked.module, "ts");
    CHECK(checked.module.functions[ts.info].max_meshes == 64);
    CHECK(checked.module.functions[ts.info].workgroup[0] == 32);

    auto const unstated = check_sources(read_prelude(), source("@task(32)"));
    CHECK(unstated.module.functions[symbol_named(unstated.module, "ts").info].max_meshes == 0);

    for (auto const attribute : {"@task(meshes = 4)", "@task(1, meshes = 0)", "@task(1, meshes = 4, meshes = 4)",
                                 "@task(1, meshes = 4, 2)", "@task(1, lines = 4)", "@task(1, meshes = 4194305)"})
        CHECK(reports_for(source(attribute)).contains("invalid-attribute-arguments"))
            .dump("attribute", cc::string(attribute));
}

TEST("sgl check - a task stage dispatches once, as the last statement of its body (CHK-485)")
{
    auto const head = cc::string_view("struct handed:\n    blue: float\n");
    auto const before = with_head(cc::string(head)
                                  + "@task(1) fun ts():\n"
                                    "    let h: handed = {blue = 1.0}\n"
                                    "    dispatch_mesh(1u, 1u, 1u, h)\n"
                                    "    let after = 1\n");
    CHECK(before.contains("calls dispatch_mesh once, as the last statement")).dump("reports", before);
    auto const none = with_head("@task(1) fun ts():\n    let x = 1\n");
    CHECK(none.contains("calls dispatch_mesh once")).dump("reports", none);
    auto const varying = with_head(cc::string(head)
                                   + "@task(4) fun ts(@local_thread_index k: int):\n"
                                     "    let h: handed = {blue = 1.0}\n"
                                     "    if k == 0:\n"
                                     "        dispatch_mesh(1u, 1u, 1u, h)\n");
    CHECK(!varying.empty()).dump("reports", varying);
}

TEST("sgl check - a payload is a struct of the program (CHK-485)")
{
    auto const scalar = with_head("@task(1) fun ts():\n    dispatch_mesh(1u, 1u, 1u, 1.0)\n");
    CHECK(scalar.contains("a payload is a struct of the program")).dump("reports", scalar);
    auto const arity = with_head("@task(1) fun ts():\n    dispatch_mesh(1u, 1u, 1u)\n");
    CHECK(arity.contains("dispatch_mesh takes three workgroup counts and the payload")).dump("reports", arity);
}

TEST("sgl check - a pipeline's mesh stage takes exactly the payload its task stage hands on (CHK-487)")
{
    auto const other
        = with_head(cc::string(k_task) + "struct other:\n    red: float\n"
                    + "@mesh(1, vertices = 3, triangles = 1)\n"
                      "fun ms2(o: other, output: mut mesh_output[corner]):\n"
                      "    output.set_counts(0u, 0u)\n"
                    + "pipeline:\n    task = ts\n    mesh = ms2\n    pixel = ps\n    format = .rgba8_unorm\n");
    CHECK(other.contains("ts hands its mesh stage handed, and ms2 takes other")).dump("reports", other);
    auto const without_task
        = with_head(cc::string(k_task) + "pipeline:\n    mesh = ms\n    pixel = ps\n    format = .rgba8_unorm\n");
    CHECK(without_task.contains("ms takes a payload, which only a task stage hands it")).dump("reports", without_task);
    auto const without_mesh
        = with_head(cc::string(k_task) + "pipeline:\n    task = ts\n    pixel = ps\n    format = .rgba8_unorm\n");
    CHECK(without_mesh.contains("a pipeline with a task stage has a mesh stage")).dump("reports", without_mesh);
}

namespace
{
constexpr auto k_faces
    = cc::string_view("struct face:\n    tint: float4\n"
                      "@mesh(1, vertices = 3, triangles = 1)\n"
                      "fun ms(output: mut mesh_output[corner, face]):\n"
                      "    output.set_counts(3u, 1u)\n"
                      "    output.primitive(0u, {tint = float4(1.0)})\n"
                      "@pixel fun tinting(c: corner, f: face) -> target => { color = c.color + f.tint }\n");
} // namespace

TEST("sgl check - a mesh output writes values per triangle, which the pixel stage takes beside its stage struct "
     "(CHK-488, CHK-489)")
{
    CHECK(with_head(cc::string(k_faces) + "pipeline:\n    mesh = ms\n    pixel = tinting\n    format = .rgba8_unorm\n")
          == "");
}

TEST("sgl check - a per-triangle struct holds no @position, and primitive needs one (CHK-488)")
{
    auto const positioned
        = with_head("struct face:\n    @position p: hpos4\n"
                    "@mesh(1, vertices = 3, triangles = 1) fun ms(output: mut mesh_output[corner, face]):\n"
                    "    output.set_counts(0u, 0u)\n");
    CHECK(positioned.contains("no @position or @per_draw member")).dump("reports", positioned);
    auto const without = with_head("@mesh(1, vertices = 3, triangles = 1) fun ms(output: mut mesh_output[corner]):\n"
                                   "    output.set_counts(1u, 1u)\n"
                                   "    output.primitive(0u, {color = float4(1.0)})\n");
    CHECK(without.contains("writes no values per triangle")).dump("reports", without);
    auto const scalar
        = with_head("@mesh(1, vertices = 3, triangles = 1) fun ms(output: mut mesh_output[corner, float]):\n"
                    "    output.set_counts(0u, 0u)\n");
    CHECK(scalar.contains("the struct it writes per triangle")).dump("reports", scalar);
}

TEST("sgl check - a pipeline's pixel stage takes exactly what its mesh stage writes per triangle (CHK-489)")
{
    auto const missing
        = with_head(cc::string(k_faces) + "pipeline:\n    mesh = ms\n    pixel = ps\n    format = .rgba8_unorm\n");
    CHECK(missing.contains("ms writes face per triangle, and ps takes none")).dump("reports", missing);
    auto const vertex
        = with_head(cc::string(k_faces)
                    + "@vertex fun vs(@vertex_index i: int) -> corner => { position = hpos4(0.0, 0.0, 0.0, "
                      "1.0), color = float4(1.0) }\n"
                    + "pipeline:\n    vertex = vs\n    pixel = tinting\n    format = .rgba8_unorm\n");
    CHECK(vertex.contains("tinting takes face per triangle, which only a mesh stage's output writes")).dump("reports", vertex);
}

TEST("sgl check - a mesh stage writes lines or triangles, and its primitives are the ones it says (CHK-480, CHK-482)")
{
    auto const lines = cc::string_view("@mesh(1, vertices = 2, lines = 1)\n"
                                       "fun ms(output: mut mesh_output[corner]):\n"
                                       "    output.set_counts(2u, 1u)\n"
                                       "    output.line(0u, uint2(0u, 1u))\n");
    CHECK(with_head(cc::string(lines) + "pipeline:\n    mesh = ms\n    pixel = ps\n    format = .rgba8_unorm\n") == "");
    // one of the two, once
    CHECK(with_head("@mesh(1, vertices = 2, lines = 1, triangles = 1)\nfun ms(output: mut mesh_output[corner]):\n"
                    "    output.set_counts(2u, 1u)\n")
              .contains("invalid-attribute-arguments"));
    // a mesh of lines writes no triangle, and one of triangles no line
    CHECK(with_head("@mesh(1, vertices = 3, lines = 1)\nfun ms(output: mut mesh_output[corner]):\n"
                    "    output.set_counts(3u, 1u)\n    output.triangle(0u, uint3(0u, 1u, 2u))\n")
              .contains("this mesh stage writes lines"));
    auto with_line = cc::string(k_mesh);
    with_line.replace_all("output.triangle(0u, uint3(0u, 1u, 2u))", "output.line(0u, uint2(0u, 1u))");
    CHECK(with_head(with_line).contains("this mesh stage writes triangles"));
    // the topology is the mesh stage's, which a pipeline does not set
    CHECK(with_head(cc::string(lines)
                    + "pipeline:\n    mesh = ms\n    pixel = ps\n    topology = .line_list\n    format = "
                      ".rgba8_unorm\n")
              .contains("sets no topology"));
}
