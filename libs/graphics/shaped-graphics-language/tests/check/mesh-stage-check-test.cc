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
    CHECK(extra.contains("vertex takes an index and the vertex written there")).dump("reports", extra);
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
