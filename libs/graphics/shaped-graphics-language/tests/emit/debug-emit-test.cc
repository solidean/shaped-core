#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::emit::target;

namespace
{
constexpr cc::string_view k_compute = "binding work:\n"
                                      "    values: mut buffer[float]\n"
                                      "\n"
                                      "@compute(64) fun cs(@thread_id id: int3){work}:\n"
                                      "    let v = work.values[id.x]\n"
                                      "    print \"v = $v at $(id.xy)\"\n"
                                      "    assert v >= 0.0, \"negative $v\"\n"
                                      "    work.values[id.x] = v * 2.0\n";

sgl::emitted_source compile(cc::string_view source, target t, cc::string_view entry = "cs")
{
    auto r = sgl::compile_to_text(
        {.source = source, .source_name = "debug.sgl", .entry_point = entry, .target = t, .diagnostics = true});
    REQUIRE(r.has_value()).dump("errors", r.has_value() ? cc::string() : r.error());
    return cc::move(r.value());
}
} // namespace

TEST("sgl emit - with diagnostics each target declares sg's debug buffer where sg binds it (EMIT-183)")
{
    CHECK(compile(k_compute, target::hlsl_dx12).text.contains("RWByteAddressBuffer sg_debug : register(u0, space11);\n"));
    CHECK(compile(k_compute, target::hlsl_vulkan).text.contains("[[vk::binding(19, 3)]] RWByteAddressBuffer sg_debug;\n"));
    CHECK(compile(k_compute, target::wgsl)
              .text.contains("@group(3) @binding(19) var<storage, read_write> sg_debug: sg_debug_buffer;\n"));
}

TEST("sgl emit - a print is one record: reserved at once, written whole where it fits, and counted where it does not")
{
    auto const hlsl = compile(k_compute, target::hlsl_dx12).text;
    CHECK(hlsl.contains("sg_debug.InterlockedAdd(0, 5, debug_at);\n"));
    CHECK(hlsl.contains("if (debug_at + 5 <= sg_debug.Load(4))\n"));
    CHECK(hlsl.contains("sg_debug.Store(12 + (debug_at + 1) * 4, 5u);\n"));
    CHECK(hlsl.contains("sg_debug.InterlockedAdd(8, 1);\n"));

    auto const wgsl = compile(k_compute, target::wgsl).text;
    CHECK(wgsl.contains("let debug_at: u32 = atomicAdd(&sg_debug.cursor, 5u);\n"));
    CHECK(wgsl.contains("sg_debug.words[debug_at + 2u] = bitcast<u32>(printed);\n"));
    CHECK(wgsl.contains("_ = atomicAdd(&sg_debug.dropped, 1u);\n"));
}

TEST("sgl emit - a failed assert writes its record and ends a compute invocation")
{
    auto const hlsl = compile(k_compute, target::hlsl_dx12).text;
    auto const failed = hlsl.find("if (!check");
    REQUIRE(failed >= 0);
    CHECK(hlsl.subview({.offset = failed, .size = hlsl.size() - failed}).contains("return;"));
}

TEST("sgl emit - each site is described: an id, its pieces, its values' shapes and its line")
{
    auto const e = compile(k_compute, target::hlsl_dx12);
    REQUIRE(e.debug_sites.size() == 2);
    auto const& print = e.debug_sites[0];
    CHECK(!print.is_assert);
    REQUIRE(print.pieces.size() == 3);
    CHECK(print.pieces[0] == "v = ");
    CHECK(print.pieces[1] == " at ");
    REQUIRE(print.values.size() == 2);
    CHECK(print.values[0].family == "float");
    CHECK(print.values[1].family == "int");
    CHECK(print.values[1].lanes == 2);
    CHECK(print.origin.at.line == 6);

    auto const& assert_site = e.debug_sites[1];
    CHECK(assert_site.is_assert);
    CHECK(assert_site.origin.at.line == 7);
    CHECK(assert_site.id != print.id);
    // the id is the site's wherever the text is written
    CHECK(compile(k_compute, target::wgsl).debug_sites[0].id == print.id);
}

TEST("sgl emit - a stage that cannot write the buffer drops its prints and declares none")
{
    constexpr auto vertex = "struct varyings:\n"
                            "    @position position: hpos4\n"
                            "\n"
                            "@vertex fun vs(@vertex_index v: int) -> varyings:\n"
                            "    print \"vertex $v\"\n"
                            "    return { position = hpos4(v as float, 0.0, 0.0, 1.0) }\n";
    auto const wgsl = compile(vertex, target::wgsl, "vs");
    CHECK(!wgsl.text.contains("sg_debug"));
    CHECK(wgsl.debug_sites.empty());
    CHECK(compile(vertex, target::hlsl_dx12, "vs").text.contains("sg_debug"));
    // vulkan writes no storage from a vertex stage without a feature sg does not ask for
    CHECK(!compile(vertex, target::hlsl_vulkan, "vs").text.contains("sg_debug"));
}

TEST("sgl emit - MSL takes sg's debug buffer at the argument table's last slot, clear of the vertex buffers")
{
    // a vertex stage's buffers start at buffer(5), so a slot just above them would be a vertex buffer's
    auto const msl = compile(k_compute, target::msl).text;
    CHECK(msl.contains("device sg_debug_buffer& sg_debug [[buffer(30)]]")).dump("text", msl);
}

TEST("sgl emit - a vertex stage's failed assert returns its vertex at a NaN position, which culls its primitives")
{
    constexpr auto vertex = "struct varyings:\n"
                            "    @position position: hpos4\n"
                            "\n"
                            "@vertex fun vs(@vertex_index v: int) -> varyings:\n"
                            "    assert v < 3, \"vertex $v\"\n"
                            "    return { position = hpos4(v as float, 0.0, 0.0, 1.0) }\n";
    for (auto const t : {target::hlsl_dx12, target::wgsl})
    {
        auto const text = compile(vertex, t, "vs").text;
        // a quiet NaN's bits, the one way every target spells a NaN
        CHECK(text.contains("2143289344")).dump("target", sgl::emit::to_string(t)).dump("text", text);
        CHECK(text.contains("culled"));
    }
}

TEST("sgl emit - a failed assert ends a patch, a candidate and a box as each stage ends: no factors, ignore, no hit")
{
    constexpr auto patch = "require tessellation_shader\n"
                           "struct control_point:\n    position: float3\n"
                           "struct tri_factors:\n"
                           "    @edge_factors edges: float[3]\n"
                           "    @inside_factors inside: float\n"
                           "@tessellation_control(partitioning = .integer, winding = .counter_clockwise)\n"
                           "fun tc(patch: control_point[3]) -> tri_factors:\n"
                           "    assert patch[0].position.x >= 0.0, \"patch at $(patch[0].position.x)\"\n"
                           "    return { edges = [1.0, 1.0, 1.0], inside = 1.0 }\n";
    auto const control = compile(patch, target::hlsl_dx12, "tc").text;
    CHECK(control.contains("culled.edges[2] = 0.0;")).dump("text", control);

    constexpr auto candidate = "require raytracing_pipeline\n"
                               "struct radiance:\n    color: float3\n"
                               "@any_hit fun keep(c: triangle_candidate, p: mut radiance) -> hit_decision:\n"
                               "    assert p.color.x >= 0.0, \"negative $(p.color.x)\"\n"
                               "    return hit_decision.accept\n";
    auto const any_hit = compile(candidate, target::hlsl_dx12, "keep").text;
    CHECK(any_hit.contains("IgnoreHit()")).dump("text", any_hit);

    constexpr auto box = "require raytracing_pipeline\n"
                         "struct sphere_hit:\n    normal: float3\n"
                         "@intersection fun sphere(b: procedural_box) -> report[sphere_hit]:\n"
                         "    assert b.primitive_index >= 0, \"box $(b.primitive_index)\"\n"
                         "    return report(1.0, sphere_hit(float3(0.0, 1.0, 0.0)))\n";
    auto const intersection = compile(box, target::hlsl_dx12, "sphere").text;
    CHECK(intersection.contains("is_hit = false")).dump("text", intersection);
}
