#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::emit::mesh_form;
using sgl::emit::target;

// The tessellation stages polyfilled as compute passes and a draw (CHK-527): the control stage's compute half with its
// pipeline's vertex stage, the prelude's passes, the evaluation stage's compute half, and the vertex half that draws.

namespace
{
constexpr auto k_program = cc::string_view(R"(require tessellation_shader

@vertex struct corner_in:
    position: float2

struct control_point:
    position: float2

struct staged:
    @position position: hpos4
    color: float4

struct tri_factors:
    @edge_factors edges: float[3]
    @inside_factors inside: float

struct quad_factors:
    @edge_factors edges: float[4]
    @inside_factors inside: float[2]
    scale: float

@vertex fun corner_vs(v: corner_in) -> control_point:
    return {position = v.position}

@tessellation_control(partitioning = .fractional_odd, winding = .clockwise)
fun tri_tc(patch: control_point[3], @primitive_id id: int) -> tri_factors:
    let f = 2.0 + (id as float)
    return {edges = [f, f, f], inside = f}

@tessellation_evaluation
fun tri_te(patch: control_point[3], f: tri_factors, @domain_location uvw: float3) -> staged:
    let p = patch[0].position * uvw.x + patch[1].position * uvw.y + patch[2].position * uvw.z
    return {position = hpos4(p.x, p.y, 0.5, 1.0), color = float4(uvw.x, uvw.y, uvw.z, f.inside)}

@tessellation_control(partitioning = .integer, winding = .counter_clockwise, max_factor = 6)
fun quad_tc(patch: control_point[4]) -> quad_factors:
    return {edges = [3.0, 3.0, 3.0, 3.0], inside = [3.0, 3.0], scale = 2.0}

@tessellation_evaluation
fun quad_te(patch: control_point[4], f: quad_factors, @domain_location uv: float2) -> staged:
    let a = patch[0].position + (patch[1].position - patch[0].position) * uv.x
    return {position = hpos4(a.x * f.scale, a.y + uv.y, 0.5, 1.0), color = float4(uv.x, uv.y, 0.0, 1.0)}
)");

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> half_of(cc::string_view entry,
                                                                  target t,
                                                                  mesh_form form,
                                                                  cc::string_view vertex = "",
                                                                  cc::string_view control = "")
{
    return sgl::compile_to_text({.source = k_program,
                                 .entry_point = entry,
                                 .target = t,
                                 .mesh = form,
                                 .polyfill_vertex = vertex,
                                 .polyfill_control = control});
}
} // namespace

TEST("sgl emit - a polyfilled tessellation's halves agree on a patch's words (CHK-527)")
{
    for (auto const t : sgl::emit::all_targets())
    {
        auto const tc = half_of("tri_tc", t, mesh_form::compute, "corner_vs");
        auto const te = half_of("tri_te", t, mesh_form::compute, "", "tri_tc");
        auto const draw = half_of("tri_te", t, mesh_form::vertex_fetch);
        REQUIRE(tc.has_value()).dump("error", tc.has_error() ? tc.error() : cc::string());
        REQUIRE(te.has_value()).dump("error", te.has_error() ? te.error() : cc::string());
        REQUIRE(draw.has_value()).dump("error", draw.has_error() ? draw.error() : cc::string());
        auto const& a = tc.value().tessellation_layout.value();
        auto const& d = te.value().tessellation_layout.value();
        CHECK(a.domain == 3);
        CHECK(a.control_points == 3);
        CHECK(a.control_words == 2);
        // three edges and the inside factor
        CHECK(a.factor_words == 4);
        CHECK(a.patch_words() == 3 * 2 + 4);
        CHECK(a.partitioning == 2);
        // EMIT-134: a clockwise control stage is D3D's `triangle_ccw`
        CHECK(a.output == 1);
        CHECK(d.patch_words() == a.patch_words());
        // a hpos4 and a float4, one word a lane
        CHECK(d.vertex_words == 8);
        CHECK(draw.value().tessellation_layout.value().vertex_words == 8);
        CHECK(tc.value().stage == sgl::check::stage::compute);
        CHECK(te.value().stage == sgl::check::stage::compute);
        CHECK(draw.value().stage == sgl::check::stage::vertex);
        CHECK(!tc.value().features.has(sgl::check::feature::tessellation_shader));
        CHECK(!te.value().features.has(sgl::check::feature::tessellation_shader));
    }

    // a quad's factors struct may carry more than its factors, which the evaluation stage reads back whole
    auto const quad = half_of("quad_te", target::msl, mesh_form::compute, "", "quad_tc");
    REQUIRE(quad.has_value()).dump("error", quad.has_error() ? quad.error() : cc::string());
    auto const& l = quad.value().tessellation_layout.value();
    CHECK(l.domain == 4);
    CHECK(l.factor_words == 4 + 2 + 1);
    CHECK(l.output == 0);
    CHECK(l.max_factor == 6);
    // a control stage that states no `max_factor` holds room for D3D's largest
    CHECK(half_of("tri_tc", target::msl, mesh_form::compute, "corner_vs").value().tessellation_layout.value().max_factor
          == 64);
}

TEST("sgl emit - a control stage's `max_factor` caps the factors it returns, natively and polyfilled (CHK-304)")
{
    // natively, every return caps each factor, and HLSL promises the cap to D3D
    auto const hlsl = half_of("quad_tc", target::hlsl_dx12, mesh_form::native);
    REQUIRE(hlsl.has_value()).dump("error", hlsl.has_error() ? hlsl.error() : cc::string());
    CHECK(hlsl.value().text.contains("[maxtessfactor(6.0)]")).dump("text", hlsl.value().text);
    CHECK(hlsl.value().text.contains("> 6.0")).dump("text", hlsl.value().text);
    auto const uncapped = half_of("tri_tc", target::hlsl_dx12, mesh_form::native);
    REQUIRE(uncapped.has_value()).dump("error", uncapped.has_error() ? uncapped.error() : cc::string());
    CHECK(!uncapped.value().text.contains("maxtessfactor")).dump("text", uncapped.value().text);
    // the polyfill's control pass stores the capped factors the evaluation stage reads back
    auto const polyfilled = half_of("quad_tc", target::msl, mesh_form::compute, "corner_vs");
    REQUIRE(polyfilled.has_value()).dump("error", polyfilled.has_error() ? polyfilled.error() : cc::string());
    CHECK(polyfilled.value().text.contains("> 6.0")).dump("text", polyfilled.value().text);
}

TEST("sgl emit - a polyfilled tessellation's compute halves bind sg's buffers at their reserved places (CHK-527)")
{
    auto const tc = half_of("tri_tc", target::msl, mesh_form::compute, "corner_vs");
    REQUIRE(tc.has_value()).dump("error", tc.has_error() ? tc.error() : cc::string());
    auto const& text = tc.value().text;
    CHECK(text.contains("kernel void tri_tc")).dump("text", text);
    CHECK(text.contains("sg_tessellation_controls [[buffer(12)]]")).dump("text", text);
    CHECK(text.contains("sg_tessellation_patches [[buffer(11)]]")).dump("text", text);
    CHECK(text.contains("sg_geometry_draw [[buffer(22)]]")).dump("text", text);
    CHECK(tc.value().is_dispatched_by_threads);
    CHECK(tc.value().workgroup[0] == sgl::check::k_tessellation_polyfill_workgroup);

    // the evaluation pass is dispatched by the scan's workgroup count, and its threads past the last point return
    auto const te = half_of("tri_te", target::msl, mesh_form::compute, "", "tri_tc");
    REQUIRE(te.has_value()).dump("error", te.has_error() ? te.error() : cc::string());
    CHECK(te.value().text.contains("sg_tessellation_points [[buffer(13)]]")).dump("text", te.value().text);
    CHECK(te.value().text.contains("sg_mesh_output [[buffer(20)]]")).dump("text", te.value().text);
    CHECK(!te.value().is_dispatched_by_threads);

    auto const draw = half_of("tri_te", target::msl, mesh_form::vertex_fetch);
    REQUIRE(draw.has_value()).dump("error", draw.has_error() ? draw.error() : cc::string());
    CHECK(draw.value().text.contains("sg_mesh_input [[buffer(21)]]")).dump("text", draw.value().text);
}

TEST("sgl emit - the tessellator's passes are the prelude's, compiled by name and listed by no program (CHK-527)")
{
    for (auto const t : sgl::emit::all_targets())
        for (auto const name : {"sg_tessellation_count", "sg_tessellation_scan", "sg_tessellation_generate"})
        {
            auto const pass = sgl::compile_to_text({.source = "", .entry_point = name, .target = t});
            REQUIRE(pass.has_value())
                .dump("pass", cc::string_view(name))
                .dump("error", pass.has_error() ? pass.error() : cc::string());
            CHECK(pass.value().stage == sgl::check::stage::compute);
        }
    auto const generate
        = sgl::compile_to_text({.source = "", .entry_point = "sg_tessellation_generate", .target = target::msl});
    CHECK(generate.value().text.contains("sg_tessellation_indices [[buffer(14)]]")).dump("text", generate.value().text);

    // an empty program names none of them among what it holds
    auto const missing = sgl::compile_to_text({.source = "", .entry_point = "nothing", .target = target::msl});
    REQUIRE(missing.has_error());
    CHECK(missing.error().contains("the source holds: none")).dump("error", missing.error());
}

TEST("sgl emit - a polyfilled evaluation stage's compute half without its control stage is refused by name (CHK-527)")
{
    auto const te = half_of("tri_te", target::msl, mesh_form::compute);
    REQUIRE(te.has_error());
    CHECK(te.error().contains("none was named")).dump("error", te.error());
    auto const tc = half_of("tri_tc", target::msl, mesh_form::compute);
    REQUIRE(tc.has_error());
    CHECK(tc.error().contains("none was named")).dump("error", tc.error());
}
