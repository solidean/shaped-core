#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::emit::mesh_form;
using sgl::emit::target;

// A geometry stage polyfilled as a compute entry point that runs its vertex stage on each corner and writes each
// primitive's slots, and the mesh polyfill's vertex entry point that draws them back (CHK-525).

namespace
{
constexpr auto k_program = cc::string_view(R"(require geometry_shader

@vertex struct corner_in:
    position: float2
    @format(.rgba8_unorm) tint: float4
    @per_instance shift: float

struct staged:
    @position position: hpos4
    color: float4

@vertex fun corner_vs(v: corner_in, @vertex_index i: int, @instance_index k: int) -> staged:
    return {position = hpos4(v.position.x + v.shift, v.position.y, i as float, k as float), color = v.tint}

@geometry(max_vertices = 6)
fun twice_gs(tri: staged[3], @primitive_id id: int, stream: mut triangle_stream[staged]):
    for i in 0 ..< 3:
        stream.emit(tri[i])
    stream.end_strip()
    if id > 100:
        return
    for i in 0 ..< 3:
        stream.emit({position = tri[i].position, color = float4(1.0)})
    stream.end_strip()

@geometry(max_vertices = 4)
fun edges_gs(tri: staged[3], stream: mut line_stream[staged]):
    for i in 0 ..< 4:
        stream.emit(tri[i % 3])

@geometry(max_vertices = 3)
fun points_gs(tri: staged[3], stream: mut point_stream[staged]):
    stream.emit(tri[0])
)");

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> half_of(cc::string_view entry,
                                                                  target t,
                                                                  mesh_form form,
                                                                  cc::string_view vertex = "corner_vs")
{
    return sgl::compile_to_text(
        {.source = k_program, .entry_point = entry, .target = t, .mesh = form, .polyfill_vertex = vertex});
}
} // namespace

TEST("sgl emit - a polyfilled geometry stage's two halves agree on a primitive's slots (CHK-525)")
{
    for (auto const t : {target::wgsl, target::hlsl_dx12, target::hlsl_vulkan, target::msl})
        for (auto const form : {mesh_form::compute, mesh_form::vertex_fetch})
        {
            auto const e = half_of("twice_gs", t, form);
            REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
            REQUIRE(e.value().mesh_layout.has_value());
            auto const& l = e.value().mesh_layout.value();
            CHECK(l.corners == 3);
            CHECK(l.max_vertices == 6);
            // one strip of 6 vertices cuts into 4 triangles, the most any order of `end_strip` makes
            CHECK(l.max_primitives == 4);
            // a hpos4 and a float4, one word a lane
            CHECK(l.vertex_words == 8);
            CHECK(l.group_words == 2 + 3 * 4 + 6 * 8);
        }
}

TEST("sgl emit - a polyfilled geometry stage's compute half reads the draw's buffers and writes sg_mesh_output")
{
    auto const msl = half_of("twice_gs", target::msl, mesh_form::compute);
    REQUIRE(msl.has_value()).dump("error", msl.has_error() ? msl.error() : cc::string());
    auto const& text = msl.value().text;
    CHECK(text.contains("kernel void twice_gs")).dump("text", text);
    CHECK(text.contains("sg_mesh_output [[buffer(20)]]")).dump("text", text);
    CHECK(text.contains("sg_geometry_draw [[buffer(22)]]")).dump("text", text);
    CHECK(text.contains("sg_geometry_indices [[buffer(23)]]")).dump("text", text);
    // one stream per vertex and one per instance, each its own buffer
    CHECK(text.contains("sg_geometry_vertices0 [[buffer(24)]]")).dump("text", text);
    CHECK(text.contains("sg_geometry_vertices1 [[buffer(25)]]")).dump("text", text);
    CHECK(text.contains("unpack_unorm4x8")).dump("text", text);
    CHECK(msl.value().stage == sgl::check::stage::compute);
    CHECK(msl.value().is_dispatched_by_threads);
    CHECK(msl.value().workgroup[0] == sgl::check::k_geometry_polyfill_workgroup);
    CHECK(!msl.value().features.has(sgl::check::feature::geometry_shader));

    // WebGPU has no geometry stage, and the compute half needs none
    auto const wgsl = half_of("twice_gs", target::wgsl, mesh_form::compute);
    REQUIRE(wgsl.has_value()).dump("error", wgsl.has_error() ? wgsl.error() : cc::string());
    CHECK(wgsl.value().text.contains("@compute @workgroup_size(64, 1, 1)")).dump("text", wgsl.value().text);
    CHECK(wgsl.value().text.contains("sg_dispatch_grid")).dump("text", wgsl.value().text);
    CHECK(!wgsl.value().text.contains("emit")).dump("text", wgsl.value().text);
}

TEST("sgl emit - a polyfilled geometry stage's vertex half is the mesh polyfill's, over the stream's struct")
{
    auto const e = half_of("twice_gs", target::msl, mesh_form::vertex_fetch, "");
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    CHECK(e.value().stage == sgl::check::stage::vertex);
    CHECK(e.value().text.contains("sg_mesh_input [[buffer(21)]]")).dump("text", e.value().text);
    CHECK(e.value().text.contains("[[vertex_id]]")).dump("text", e.value().text);
}

TEST("sgl emit - a polyfilled geometry stage that appends lines draws two corners a primitive (CHK-525)")
{
    auto const e = half_of("edges_gs", target::hlsl_dx12, mesh_form::compute);
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    auto const& l = e.value().mesh_layout.value();
    CHECK(l.corners == 2);
    CHECK(l.max_primitives == 3);
}

TEST("sgl emit - a geometry stage the polyfill does not carry yet is refused by name, not guessed (CHK-525)")
{
    auto const points = half_of("points_gs", target::msl, mesh_form::compute);
    REQUIRE(points.has_error());
    CHECK(points.error().contains("appends points")).dump("error", points.error());

    auto const no_vertex = half_of("twice_gs", target::msl, mesh_form::compute, "");
    REQUIRE(no_vertex.has_error());
    CHECK(no_vertex.error().contains("none was named")).dump("error", no_vertex.error());

    auto const not_a_vertex = half_of("twice_gs", target::msl, mesh_form::compute, "edges_gs");
    REQUIRE(not_a_vertex.has_error());
    CHECK(not_a_vertex.error().contains("no vertex entry point named 'edges_gs'")).dump("error", not_a_vertex.error());
}
