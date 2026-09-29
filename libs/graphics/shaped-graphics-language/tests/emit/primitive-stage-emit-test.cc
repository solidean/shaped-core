#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

// The geometry and tessellation stages as HLSL (EMIT-123), and the two targets that refuse them.

namespace
{
constexpr auto k_program = cc::string_view(
    "require geometry_shader\n"
    "require tessellation_shader\n"
    "binding params:\n    level: float\n"
    "struct control_point:\n    position: float3\n"
    "struct varyings:\n    @position position: hpos4\n    color: float3\n"
    "struct tri_factors:\n"
    "    @edge_factors edges: float[3]\n"
    "    @inside_factors inside: float\n"
    "    bulge: float\n"
    "@tessellation_control(partitioning = .fractional_odd, winding = .counter_clockwise)\n"
    "fun tc(patch: control_point[3]){params} -> tri_factors:\n"
    "    return { edges = [params.level, params.level, params.level], inside = params.level, bulge = 0.0 }\n"
    "@tessellation_evaluation\n"
    "fun te(patch: control_point[3], f: tri_factors, @domain_location uvw: float3) -> varyings:\n"
    "    let p = patch[0].position * uvw.x + patch[1].position * uvw.y + patch[2].position * uvw.z\n"
    "    return { position = hpos4(p.x, p.y, p.z + f.bulge, 1.0), color = uvw }\n"
    "@geometry(max_vertices = 6)\n"
    "fun gs(tri: varyings[3], @primitive_id prim: int, stream: mut triangle_stream[varyings]):\n"
    "    for i in 0 ..< 3:\n"
    "        stream.emit(tri[i])\n"
    "    stream.end_strip()\n");

constexpr isize k_control = 0;
constexpr isize k_evaluation = 1;
constexpr isize k_geometry = 2;

cc::string text_of(isize entry, target t)
{
    auto const e = emit_source(k_program, entry, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

TEST("sgl emit - a geometry stage is a maxvertexcount function appending to its stream")
{
    auto const hlsl = text_of(k_geometry, target::hlsl_dx12);
    CHECK(hlsl.contains("[maxvertexcount(6)]\n"
                        "void gs(triangle varyings tri[3], uint prim_in : SV_PrimitiveID, "
                        "inout TriangleStream<varyings> stream)\n"));
    CHECK(hlsl.contains("stream.Append("));
    CHECK(hlsl.contains("    stream.RestartStrip();\n"));
}

TEST("sgl emit - a control stage is a hull passthrough and its patch-constant function")
{
    auto const hlsl = text_of(k_control, target::hlsl_dx12);
    CHECK(hlsl.contains("    float edges[3] : SV_TessFactor;\n    float inside : SV_InsideTessFactor;\n"));
    CHECK(hlsl.contains("tri_factors tc_patch(InputPatch<control_point, 3> patch)\n"));
    CHECK(hlsl.contains("[domain(\"tri\")]\n"
                        "[partitioning(\"fractional_odd\")]\n"
                        "[outputtopology(\"triangle_cw\")]\n" // EMIT-134: counter-clockwise, in HLSL's mirror
                        "[outputcontrolpoints(3)]\n"
                        "[patchconstantfunc(\"tc_patch\")]\n"
                        "control_point tc(InputPatch<control_point, 3> patch, uint point_index : "
                        "SV_OutputControlPointID)\n"
                        "{\n"
                        "    return patch[point_index];\n"
                        "}\n"));

    // vulkan: the factors' own members sit after the control point's locations
    auto const vulkan = text_of(k_control, target::hlsl_vulkan);
    CHECK(vulkan.contains("[[vk::location(1)]] float bulge : SGL1;\n"));
}

TEST("sgl emit - an evaluation stage takes its patch, its factors and its domain location")
{
    auto const hlsl = text_of(k_evaluation, target::hlsl_dx12);
    CHECK(hlsl.contains("[domain(\"tri\")]\n"
                        "varyings te(const OutputPatch<control_point, 3> patch, tri_factors f, "
                        "float3 uvw_in : SV_DomainLocation)\n"));
}

TEST("sgl emit - WebGPU and Metal have neither stage, and refuse each by its feature")
{
    for (auto const entry : {k_control, k_evaluation, k_geometry})
    {
        auto const wgsl = sgl::emit::dump_errors(emit_source(k_program, entry, target::wgsl));
        CHECK(wgsl.contains("target-lacks-feature"));
        CHECK(wgsl.contains("which WebGPU does not have"));
        auto const msl = sgl::emit::dump_errors(emit_source(k_program, entry, target::msl));
        CHECK(msl.contains("target-lacks-feature"));
        CHECK(msl.contains("which Metal does not have"));
    }
}
