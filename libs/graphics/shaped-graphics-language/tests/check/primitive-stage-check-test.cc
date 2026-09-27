#include "check-test-support.hh"

using namespace sgl_test;

// The geometry and tessellation stages, CHK-301 to CHK-307: each is a C++ test rather than a corpus entry,
// since WGSL and MSL refuse both stages and a corpus entry point is emitted on every target.

namespace
{
/// The structs every stage below passes on, and the features they need.
constexpr auto k_head = cc::string_view("require geometry_shader\n"
                                        "require tessellation_shader\n"
                                        "@vertex struct vin:\n    position: float3\n"
                                        "struct control_point:\n    position: float3\n"
                                        "struct varyings:\n    @position position: hpos4\n    color: float3\n"
                                        "struct tri_factors:\n"
                                        "    @edge_factors edges: float[3]\n"
                                        "    @inside_factors inside: float\n"
                                        "    bulge: float\n"
                                        "@pixel struct target:\n    color: float4\n");

/// A stage of each kind that fits the ones around it.
constexpr auto k_stages = cc::string_view(
    "@vertex fun vs(v: vin) -> control_point => { position = v.position }\n"
    "@tessellation_control(partitioning = .fractional_odd, winding = .counter_clockwise)\n"
    "fun tc(patch: control_point[3]) -> tri_factors:\n"
    "    return { edges = [1.0, 1.0, 1.0], inside = 1.0, bulge = patch[0].position.x }\n"
    "@tessellation_evaluation\n"
    "fun te(patch: control_point[3], f: tri_factors, @domain_location uvw: float3) -> varyings:\n"
    "    let p = patch[0].position * uvw.x + patch[1].position * uvw.y + patch[2].position * uvw.z\n"
    "    return { position = hpos4(p.x, p.y, p.z + f.bulge, 1.0), color = uvw }\n"
    "@geometry(max_vertices = 3)\n"
    "fun gs(tri: varyings[3], @primitive_id prim: int, stream: mut triangle_stream[varyings]):\n"
    "    for i in 0 ..< 3:\n"
    "        stream.emit(tri[i])\n"
    "    stream.end_strip()\n"
    "@pixel fun ps(p: varyings) -> target => { color = float4(p.color.x, p.color.y, p.color.z, 1.0) }\n");

cc::string with_head(cc::string_view program)
{
    return reports_for(cc::string(k_head) + program);
}

cc::string with_stages(cc::string_view program)
{
    return reports_for(cc::string(k_head) + k_stages + program);
}
} // namespace

TEST("sgl check - the stages of a tessellated, geometry-shaded pipeline check as they are")
{
    CHECK(with_stages("") == "");
}

TEST("sgl check - a geometry stage takes one primitive's vertices and appends to a stream")
{
    constexpr auto gs = "@geometry(max_vertices = 3)\nfun g({}):\n    {}\n";
    auto const reports = [&](cc::string_view parameters, cc::string_view body = "stream.end_strip()")
    { return with_head(cc::format(gs, parameters, body)); };

    CHECK(reports("tri: varyings[3], stream: mut triangle_stream[varyings]") == "");
    CHECK(reports("tri: varyings[5], stream: mut triangle_stream[varyings]")
              .contains("a primitive is 1, 2, 3, 4 or 6 vertices"));
    CHECK(reports("tri: varyings[3], stream: triangle_stream[varyings]").contains("the stream is `mut`"));
    CHECK(reports("tri: varyings[3]", "return").contains("last the stream it appends to"));
    CHECK(reports("tri: control_point[3], stream: mut triangle_stream[control_point]")
              .contains("what a geometry stage appends reaches the rasterizer, so it has one @position field"));
    CHECK(reports("tri: varyings[3], stream: mut triangle_stream[varyings]", "stream.emit(tri[0].color)")
              .contains("the vertex it appends"));

    // CHK-301: an int literal from 1 to 1024
    CHECK(with_head("@geometry(max_vertices = 0)\nfun g(tri: varyings[3], stream: mut triangle_stream[varyings]):\n"
                    "    stream.end_strip()\n")
              .contains("an int from 1 to 1024"));
    CHECK(with_head("@geometry(max_vertices = 3)\nfun g(tri: varyings[3], stream: mut triangle_stream[varyings]) -> "
                    "int:\n"
                    "    return 1\n")
              .contains("a @geometry fun returns nothing"));
}

TEST("sgl check - a stream is no value, and only a geometry stage appends to one")
{
    CHECK(with_head("fun f(s: triangle_stream[varyings]) -> int => 1\n") != "");
    CHECK(with_head("struct holder:\n    s: triangle_stream[varyings]\n") != "");
    CHECK(with_head("@geometry(max_vertices = 1)\nfun g(p: varyings[1], stream: mut point_stream[varyings]):\n"
                    "    stream.flush()\n")
              .contains("a stream has `emit(v)` and `end_strip()`"));
}

TEST("sgl check - a control stage names its partitioning and winding, takes a patch and returns its factors")
{
    constexpr auto tc = "@tessellation_control{}\nfun tc(patch: control_point[{}]) -> {}:\n    return {}\n";
    constexpr auto mode = "(partitioning = .integer, winding = .clockwise)";
    constexpr auto factors = "{ edges = [1.0, 1.0, 1.0], inside = 1.0, bulge = 0.0 }";
    CHECK(with_head(cc::format(tc, mode, 3, "tri_factors", factors)) == "");
    CHECK(with_head(cc::format(tc, "(partitioning = .integer)", 3, "tri_factors", factors))
              .contains("`winding = .clockwise` or `.counter_clockwise`"));
    CHECK(with_head(cc::format(tc, "(partitioning = .pow2, winding = .clockwise)", 3, "tri_factors", factors))
              .contains("@tessellation_control takes `partitioning = .integer`"));
    CHECK(
        with_head(cc::format(tc, mode, 33, "tri_factors", factors)).contains("a patch holds from 1 to 32 control points"));
    CHECK(with_head(cc::format(tc, mode, 3, "varyings",
                               "{ position = hpos4(0.0, 0.0, 0.0, 1.0), color = float3(0.0, 0.0, 0.0) }"))
              .contains("is a factors struct, with one @edge_factors member"));
}

TEST("sgl check - a factors struct's edges say its domain, and the domain its inside factors")
{
    constexpr auto tc = "{}\n@tessellation_control(partitioning = .integer, winding = .clockwise)\n"
                        "fun tc(patch: control_point[4]) -> quad_factors:\n    return {}\n";
    CHECK(with_head(cc::format(tc, "struct quad_factors:\n    @edge_factors e: float[4]\n    @inside_factors i: float[2]",
                               "{ e = float[4].filled(1.0), i = [1.0, 1.0] }"))
          == "");
    CHECK(with_head(cc::format(tc, "struct quad_factors:\n    @edge_factors e: float[4]\n    @inside_factors i: float",
                               "{ e = float[4].filled(1.0), i = 1.0 }"))
              .contains("a domain of 4 edges has one @inside_factors member, of float[2]"));
    CHECK(with_head(cc::format(tc, "struct quad_factors:\n    @edge_factors e: float[2]\n    @inside_factors i: float",
                               "{ e = [1.0, 1.0], i = 1.0 }"))
              .contains("isolines have no @inside_factors member"));
    CHECK(with_head(cc::format(tc, "struct quad_factors:\n    @edge_factors e: float[5]", "{ e = float[5].filled(1.0) }"))
              .contains("one @edge_factors member of float[2], float[3] or float[4]"));
}

TEST("sgl check - an evaluation stage takes the patch, its factors and where in the domain it runs")
{
    constexpr auto te = "@tessellation_evaluation\nfun te(patch: control_point[3], f: tri_factors{}) -> varyings:\n"
                        "    return {{ position = hpos4(0.0, 0.0, 0.0, 1.0), color = float3(0.0, 0.0, 0.0) }}\n";
    CHECK(with_head(cc::format(te, ", @domain_location uvw: float3")) == "");
    CHECK(with_head(cc::format(te, "")).contains("takes `@domain_location`"));
    CHECK(with_head(cc::format(te, ", @domain_location uv: float2"))
              .contains("the domain location of a triangle is a float3"));
}

TEST("sgl check - each stage needs its feature, and a stage input is taken only by its stages")
{
    auto const bare = reports_for(cc::string(k_head.subview(k_head.find("@vertex struct"))) + k_stages);
    CHECK(bare.contains("needs geometry_shader"));
    CHECK(bare.contains("needs tessellation_shader"));
    CHECK(with_head("@tessellation_evaluation\nfun te(patch: control_point[3], f: tri_factors, @domain_location uvw: "
                    "float3, @primitive_id p: int) -> varyings:\n"
                    "    return { position = hpos4(0.0, 0.0, 0.0, 1.0), color = uvw }\n")
          == "");
    CHECK(with_head("@pixel fun p(v: varyings, @domain_location uvw: float3) -> target => { color = float4(1.0, 1.0, "
                    "1.0, "
                    "1.0) }\n")
              .contains("@domain_location"));
}

TEST("sgl check - a pipeline chains its stages, and the tessellation stages draw the patch they take")
{
    constexpr auto format = "    color_targets.color.format = .rgba8_unorm\n";
    CHECK(with_stages(cc::format("pipeline full:\n    vertex = vs\n    tessellation_control = tc\n"
                                 "    tessellation_evaluation = te\n    geometry = gs\n    pixel = ps\n{}",
                                 format))
          == "");
    CHECK(with_stages(cc::format("pipeline short = (vs, tc, te, ps):\n{}", format)) == "");

    // CHK-307: the two tessellation stages come together
    CHECK(with_stages(
              cc::format("pipeline half:\n    vertex = vs\n    tessellation_control = tc\n    pixel = ps\n{}", format))
              .contains("the two tessellation stages come together"));
    // their topology is the patch, which no setting names
    CHECK(with_stages(cc::format("pipeline p = (vs, tc, te, ps):\n{}    topology = .triangle_list\n", format))
              .contains("the tessellation stages draw patches of 3, so a pipeline with them sets no topology"));
    CHECK(with_stages(cc::format("pipeline p = (vs, tc, te, ps):\n{}    patch_control_points = 3\n", format))
              .contains("sets no patch_control_points"));
    CHECK(with_stages(
              "fun f() -> int => 1\n@vertex fun vp(v: vin) -> varyings => { position = hpos4(0.0, 0.0, 0.0, 1.0), "
              "color = v.position }\n"
              "pipeline p = (vp, ps):\n    color_targets.color.format = .rgba8_unorm\n    topology = .patch_list\n")
              .contains("a pipeline draws patches through its tessellation stages, and this one has none"));

    // what the vertex stage returns is the patch's control point, member for member
    CHECK(with_stages(cc::format("@vertex fun vv(v: vin) -> varyings => {{ position = hpos4(0.0, 0.0, 0.0, 1.0), color "
                                 "= "
                                 "v.position }}\npipeline p = (vv, tc, te, ps):\n{}",
                                 format))
              .contains("the stages pass one interface, member for member"));
    // what reaches the rasterizer has its @position
    CHECK(with_stages(cc::format("pipeline p = (vs, ps):\n{}", format))
              .contains("vs hands the rasterizer control_point, which has no @position field"));
}

TEST("sgl check - a geometry stage takes the primitive the pipeline assembles")
{
    constexpr auto program = "@vertex fun vp(v: vin) -> varyings => {{ position = hpos4(0.0, 0.0, 0.0, 1.0), color = "
                             "v.position }}\n"
                             "pipeline p = (vp, gs, ps):\n    color_targets.color.format = .rgba8_unorm\n{}";
    CHECK(with_stages(cc::format(program, "")) == "");
    CHECK(with_stages(cc::format(program, "    topology = .triangle_strip\n")) == "");
    CHECK(with_stages(cc::format(program, "    topology = .line_list\n"))
              .contains("gs takes a primitive of 3 vertices, and topology .line_list assembles primitives of 2"));
}
