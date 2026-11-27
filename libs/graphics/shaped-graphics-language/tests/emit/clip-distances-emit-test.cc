#include "../check/check-test-support.hh"
#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::check::feature;
using sgl::emit::native_route;
using sgl::emit::target;

// `@clip_distances` (CHK-571 to CHK-575, EMIT-290): where the field stands, what it refuses, the feature it needs,
// and what each target writes.
// What it cuts on a device is sg's pixel-semantics-test, on every route.

namespace
{
constexpr auto k_program = cc::string_view(R"(require clip_distances

binding clip:
    planes: float4[6]

@vertex struct corner_in:
    position: float3
    uv: float2

struct varyings:
    @position position: hpos4
    @clip_distances planes: float[3]
    uv: float2

struct wide:
    @position position: hpos4
    @clip_distances planes: float[6]

@vertex fun three_vs(v: corner_in){clip} -> varyings:
    let world = float4(v.position.x, v.position.y, v.position.z, 1.0)
    let mut planes = float[3].filled(0.0)
    for i in 0 ..< 3:
        planes[i] = dot(clip.planes[i], world)
    return {position = hpos4(world.x, world.y, world.z, 1.0), planes = planes, uv = v.uv}

@vertex fun six_vs(v: corner_in){clip} -> wide:
    let world = float4(v.position.x, v.position.y, v.position.z, 1.0)
    let mut planes = float[6].filled(0.0)
    for i in 0 ..< 6:
        planes[i] = dot(clip.planes[i], world)
    return {position = hpos4(world.x, world.y, world.z, 1.0), planes = planes}

@pixel fun shade_ps(p: varyings) -> float4 => float4(p.uv.x, p.uv.y, 0.0, 1.0)
)");

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> compiled(cc::string_view entry,
                                                                   target t,
                                                                   native_route native = native_route::text)
{
    return sgl::compile_to_text({.source = k_program, .entry_point = entry, .target = t, .native = native});
}

/// The source of a vertex stage returning `varyings`, whose struct and body the caller writes.
[[nodiscard]] cc::string vertex_source(cc::string_view fields, cc::string_view result)
{
    return cc::format("require clip_distances\n"
                      "struct varyings:\n"
                      "    @position position: hpos4\n"
                      "{}"
                      "@vertex fun vs(@vertex_index i: int) -> varyings:\n"
                      "    return {}\n",
                      fields, result);
}

/// The last stage before the rasterizer of each kind, each writing two lanes and a member after them.
constexpr auto k_stages = cc::string_view(R"(require clip_distances, geometry_shader, tessellation_shader, mesh_shader

@vertex struct corner_in:
    position: float2

struct control_point:
    position: float2

struct clipped:
    @position position: hpos4
    @clip_distances planes: float[2]
    tag: float

struct quad_factors:
    @edge_factors edges: float[4]
    @inside_factors inside: float[2]

@vertex fun corner_vs(v: corner_in) -> control_point:
    return {position = v.position}

@tessellation_control(partitioning = .integer, winding = .counter_clockwise)
fun quad_tc(patch: control_point[4]) -> quad_factors:
    return {edges = [4.0, 4.0, 4.0, 4.0], inside = [4.0, 4.0]}

@tessellation_evaluation
fun quad_te(patch: control_point[4], f: quad_factors, @domain_location uv: float2) -> clipped:
    let p = patch[0].position + (patch[2].position - patch[0].position) * uv
    return {position = hpos4(p.x, p.y, 0.5, 1.0), planes = [1.0, p.x], tag = 3.0}

struct plain:
    @position position: hpos4

@vertex fun plain_vs(v: corner_in) -> plain:
    return {position = hpos4(v.position.x, v.position.y, 0.5, 1.0)}

@geometry(max_vertices = 3)
fun clip_gs(tri: plain[3], stream: mut triangle_stream[clipped]):
    for i in 0 ..< 3:
        let p = tri[i].position
        stream.emit({position = p, planes = [p.x, 1.0], tag = 3.0})

@mesh(1, vertices = 3, triangles = 1) fun clip_ms(output: mut mesh_output[clipped]):
    output.set_counts(3u, 1u)
    output.vertex(0u, {position = hpos4(-1.0, -1.0, 0.0, 1.0), planes = [1.0, -1.0], tag = 3.0})
    output.vertex(1u, {position = hpos4(3.0, -1.0, 0.0, 1.0), planes = [1.0, 3.0], tag = 3.0})
    output.vertex(2u, {position = hpos4(-1.0, 3.0, 0.0, 1.0), planes = [1.0, -1.0], tag = 3.0})
    output.triangle(0u, uint3(0u, 1u, 2u))

@pixel fun clip_ps(p: clipped) -> float4 => float4(p.tag, 0.0, 0.0, 1.0)

pipeline tessellated:
    vertex = corner_vs
    tessellation_control = quad_tc
    tessellation_evaluation = quad_te
    pixel = clip_ps
    format = .rgba32_float

pipeline geometric:
    vertex = plain_vs
    geometry = clip_gs
    pixel = clip_ps
    format = .rgba32_float

pipeline meshed:
    mesh = clip_ms
    pixel = clip_ps
    format = .rgba32_float
)");

/// The last stages of `k_stages`, by name.
constexpr cc::string_view k_last_stages[] = {"quad_te", "clip_gs", "clip_ms"};

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> staged(cc::string_view entry,
                                                                 target t,
                                                                 native_route native = native_route::text)
{
    return sgl::compile_to_text({.source = k_stages, .entry_point = entry, .target = t, .native = native});
}

/// `k_stages` with `from` replaced by `to`, which the caller states is there.
[[nodiscard]] cc::string stages_with(cc::string_view from, cc::string_view to)
{
    auto const at = k_stages.find(from);
    CC_ASSERT(at >= 0, "the replaced text is in k_stages");
    return cc::format("{}{}{}", k_stages.subview(cc::offset_size{.offset = 0, .size = at}), to,
                      k_stages.subview(at + from.size()));
}
} // namespace

TEST("sgl check - @clip_distances is one float[N] field of what a vertex stage hands the rasterizer, N from 1 to 8")
{
    // CHK-571
    auto const eight
        = reports_for(vertex_source("    @clip_distances planes: float[8]\n", "{position = hpos4(0.0, 0.0, 0.0, 1.0), "
                                                                              "planes = float[8].filled(1.0)}"));
    CHECK(eight == "").dump("report", eight);
    for (auto const field : {"    @clip_distances planes: float[9]\n", "    @clip_distances planes: float3\n",
                             "    @clip_distances planes: int[2]\n"})
    {
        auto const report = reports_for(vertex_source(field, "undefined()"));
        CHECK(report.contains("is a float[N] with N from 1 to 8")).dump("report", report).dump("field", field);
    }
    auto const twice = reports_for(
        vertex_source("    @clip_distances a: float[2]\n    @clip_distances b: float[2]\n", "undefined()"));
    CHECK(twice.contains("at most one @clip_distances field")).dump("report", twice);
    auto const interpolated
        = reports_for(vertex_source("    @clip_distances @interpolate(.flat) a: float[2]\n", "undefined()"));
    CHECK(interpolated.contains("takes no @interpolate and no @per_draw")).dump("report", interpolated);
    auto const argued = reports_for(vertex_source("    @clip_distances(2) a: float[2]\n", "undefined()"));
    CHECK(argued.contains("invalid-attribute-arguments")).dump("report", argued);
    // a struct that reaches no rasterizer holds none
    auto const unrasterized = reports_for("require clip_distances\n"
                                          "@vertex struct corner_in:\n"
                                          "    @clip_distances planes: float[2]\n"
                                          "@vertex fun vs(v: corner_in) -> hpos4 => hpos4(0.0, 0.0, 0.0, 1.0)\n");
    CHECK(unrasterized.contains("reaches no rasterizer, so @clip_distances means nothing on it")).dump("report", unrasterized);
}

TEST("sgl check - clip distances need clip_distances of the stage that writes them, and no pixel stage reads them")
{
    // CHK-575
    auto const undeclared = reports_for("struct varyings:\n"
                                        "    @position position: hpos4\n"
                                        "    @clip_distances planes: float[2]\n"
                                        "@vertex fun vs(@vertex_index i: int) -> varyings:\n"
                                        "    return undefined()\n");
    CHECK(undeclared.contains("vs needs clip_distances")).dump("report", undeclared);
    // CHK-574: WGSL hands a pixel stage none, so no target reads them there
    auto const read = reports_for("require clip_distances\n"
                                  "struct varyings:\n"
                                  "    @position position: hpos4\n"
                                  "    @clip_distances planes: float[2]\n"
                                  "@pixel fun ps(p: varyings) -> float4 => float4(p.planes[0])\n");
    CHECK(read.contains("unsupported-yet user:[p.planes] a read of clip distances in a pixel stage")).dump("report", read);
}

TEST("sgl check - the last stage before the rasterizer writes clip distances: a vertex, evaluation, geometry or mesh "
     "stage")
{
    // CHK-571: each of k_stages' pipelines, its last stage writing two lanes, checks clean
    auto const clean = reports_for(k_stages);
    CHECK(clean == "").dump("report", clean);
    // CHK-575: and each such stage needs clip_distances, which the others do not
    for (auto const entry : k_last_stages)
    {
        auto const e = staged(entry, target::hlsl_dx12);
        REQUIRE(e.has_value()).dump("entry", entry).dump("error", e.has_error() ? e.error() : cc::string());
        CHECK(e.value().features.has(feature::clip_distances)).dump("entry", entry);
    }
    for (auto const entry : {"corner_vs", "quad_tc", "plain_vs"})
    {
        auto const e = staged(entry, target::hlsl_dx12);
        REQUIRE(e.has_value()).dump("entry", entry).dump("error", e.has_error() ? e.error() : cc::string());
        CHECK(!e.value().features.has(feature::clip_distances)).dump("entry", entry);
    }
    auto const undeclared = reports_for(stages_with("require clip_distances, ", "require "));
    for (auto const entry : k_last_stages)
        CHECK(undeclared.contains(cc::format("{} needs clip_distances", entry))).dump("report", undeclared);

    // CHK-571: what a geometry, evaluation or mesh stage hands the rasterizer is judged as a vertex stage's
    auto const nine = reports_for(stages_with("@clip_distances planes: float[2]", "@clip_distances planes: float[9]"));
    CHECK(nine.contains("is a float[N] with N from 1 to 8")).dump("report", nine);
    auto const twice
        = reports_for(stages_with("    @clip_distances planes: float[2]\n", "    @clip_distances planes: float[2]\n    "
                                                                            "@clip_distances more: float[1]\n"));
    CHECK(twice.contains("what a geometry stage appends is a struct with at most one @clip_distances field"))
        .dump("report", twice);
    CHECK(twice.contains("what a mesh stage writes is a struct with at most one @clip_distances field")).dump("report", twice);
    CHECK(twice.contains("a @tessellation_evaluation fun returns a struct with at most one @clip_distances field"))
        .dump("report", twice);
    auto const flat = reports_for(stages_with("@clip_distances planes", "@clip_distances @interpolate(.flat) planes"));
    CHECK(flat.contains("takes no @interpolate and no @per_draw")).dump("report", flat);
}

TEST("sgl check - a stage another stage follows hands on no clip distances, and what reaches no rasterizer holds none")
{
    // CHK-573: a vertex stage before a geometry stage hands on no clip distance
    auto const handed = reports_for(R"(require clip_distances, geometry_shader
struct varyings:
    @position position: hpos4
    @clip_distances planes: float[2]
@vertex fun vs(@vertex_index i: int) -> varyings:
    return {position = hpos4(0.0, 0.0, 0.0, 1.0), planes = float[2].filled(1.0)}
struct gout:
    @position position: hpos4
@geometry(max_vertices = 3) fun gs(v: varyings[3], out: mut triangle_stream[gout]):
    for i in 0 ..< 3:
        out.emit({position = v[i].position})
@pixel fun ps(p: gout) -> float4 => float4(1.0)
pipeline p:
    vertex = vs
    geometry = gs
    pixel = ps
    format = .rgba8_unorm
)");
    // the geometry stage that takes them is refused as an entry point, whatever pipeline names it
    CHECK(handed.contains("invalid-entry-point")).dump("report", handed);
    CHECK(handed.contains("'planes' reaches the geometry stage from the stage before it, and only the last stage "
                          "before the rasterizer writes clip distances"))
        .dump("report", handed);

    // CHK-573: and one that takes the same members with no attribute, which no entry point refuses, the pipeline does
    auto const unmarked = reports_for(R"(require clip_distances, geometry_shader
struct varyings:
    @position position: hpos4
    @clip_distances planes: float[2]
struct taken:
    @position position: hpos4
    planes: float[2]
@vertex fun vs(@vertex_index i: int) -> varyings:
    return {position = hpos4(0.0, 0.0, 0.0, 1.0), planes = float[2].filled(1.0)}
struct gout:
    @position position: hpos4
@geometry(max_vertices = 3) fun gs(v: taken[3], out: mut triangle_stream[gout]):
    for i in 0 ..< 3:
        out.emit({position = v[i].position})
@pixel fun ps(p: gout) -> float4 => float4(1.0)
pipeline p:
    vertex = vs
    geometry = gs
    pixel = ps
    format = .rgba8_unorm
)");
    CHECK(unmarked.contains("invalid-pipeline")).dump("report", unmarked);
    CHECK(unmarked.contains("vs hands its @clip_distances field 'planes' to a geometry stage, and only what reaches "
                            "the rasterizer clips"))
        .dump("report", unmarked);

    // CHK-573: an evaluation stage before a geometry stage is no last stage either, which the pipeline says of one
    // whose geometry stage takes the same members with no attribute
    auto const evaluated = reports_for(stages_with("pipeline geometric:\n"
                                                   "    vertex = plain_vs\n"
                                                   "    geometry = clip_gs\n",
                                                   "struct taken:\n"
                                                   "    @position position: hpos4\n"
                                                   "    planes: float[2]\n"
                                                   "    tag: float\n"
                                                   "struct after:\n"
                                                   "    @position position: hpos4\n"
                                                   "@geometry(max_vertices = 3)\n"
                                                   "fun after_gs(tri: taken[3], stream: mut triangle_stream[after]):\n"
                                                   "    for i in 0 ..< 3:\n"
                                                   "        stream.emit({position = tri[i].position})\n"
                                                   "@pixel fun after_ps(p: after) -> float4 => float4(1.0)\n"
                                                   "pipeline evaluated_geometry:\n"
                                                   "    vertex = corner_vs\n"
                                                   "    tessellation_control = quad_tc\n"
                                                   "    tessellation_evaluation = quad_te\n"
                                                   "    geometry = after_gs\n"
                                                   "    pixel = after_ps\n"
                                                   "    format = .rgba32_float\n"
                                                   "pipeline geometric:\n"
                                                   "    vertex = plain_vs\n"
                                                   "    geometry = clip_gs\n"));
    CHECK(evaluated.contains("invalid-pipeline")).dump("report", evaluated);
    CHECK(evaluated.contains("quad_te hands its @clip_distances field 'planes' to a geometry stage"))
        .dump("report", evaluated);

    // a control stage's factors, a mesh stage's payload and what it writes per triangle reach no rasterizer
    auto const factors
        = reports_for(stages_with("    @inside_factors inside: float[2]\n", "    @inside_factors inside: float[2]\n"
                                                                            "    @clip_distances planes: float[1]\n"));
    CHECK(factors.contains("'planes' reaches no rasterizer, so @clip_distances means nothing on it")).dump("report", factors);
    auto const per_triangle = reports_for(R"(require clip_distances, mesh_shader
struct corner:
    @position position: hpos4
struct face:
    @clip_distances planes: float[1]
@mesh(1, vertices = 3, triangles = 1)
fun ms(output: mut mesh_output[corner, face]):
    output.set_counts(3u, 1u)
    output.triangle(0u, uint3(0u, 1u, 2u))
)");
    CHECK(per_triangle.contains("'planes' is written per triangle, and clip distances are a vertex's"))
        .dump("report", per_triangle);
    auto const payload = reports_for(R"(require clip_distances, mesh_shader
struct corner:
    @position position: hpos4
struct handed:
    @clip_distances planes: float[1]
@mesh(1, vertices = 3, triangles = 1)
fun ms(h: handed, output: mut mesh_output[corner]):
    output.set_counts(3u, 1u)
    output.triangle(0u, uint3(0u, 1u, 2u))
)");
    CHECK(payload.contains("'planes' is a task stage's payload, which reaches no rasterizer")).dump("report", payload);
}

TEST("sgl emit - an evaluation, geometry or mesh stage writes its clip distances as a vertex stage does, on each "
     "target")
{
    // EMIT-290: HLSL a float per lane, on the domain stage's output, each appended vertex and each mesh vertex
    for (auto const entry : k_last_stages)
        for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
        {
            auto const e = staged(entry, t);
            REQUIRE(e.has_value()).dump("entry", entry).dump("error", e.has_error() ? e.error() : cc::string());
            CHECK(e.value().text.contains("    float planes_0 : SV_ClipDistance0;\n    float planes_1 : "
                                          "SV_ClipDistance1;\n"))
                .dump("text", e.value().text);
        }
    // MSL's mesh vertex holds the array, filled a lane at a time; Metal has no geometry or tessellation stage
    auto const msl = staged("clip_ms", target::msl);
    REQUIRE(msl.has_value()).dump("error", msl.has_error() ? msl.error() : cc::string());
    CHECK(msl.value().text.contains("    float planes [[clip_distance]] [2];\n")).dump("text", msl.value().text);
    CHECK(msl.value().text.contains(".planes[1] = ")).dump("text", msl.value().text);
    for (auto const entry : {"quad_te", "clip_gs"})
    {
        auto const e = staged(entry, target::msl);
        REQUIRE(e.has_error());
        CHECK(e.error().contains("target-lacks-feature")).dump("error", e.error());
    }
    // the pixel stage after any of them is the vertex stage's: the lanes left out where links match by location
    auto const vulkan_ps = staged("clip_ps", target::hlsl_vulkan);
    REQUIRE(vulkan_ps.has_value()).dump("error", vulkan_ps.has_error() ? vulkan_ps.error() : cc::string());
    CHECK(!vulkan_ps.value().text.contains("planes")).dump("text", vulkan_ps.value().text);
}

TEST("sgl emit - no polyfill carries clip distances yet, and each form of one refuses them")
{
    // CHK-571: the polyfills' vertex half would write them, which none does yet
    struct form
    {
        cc::string_view entry;
        sgl::emit::mesh_form mesh;
        cc::string_view vertex;
        cc::string_view control;
    };
    form const forms[] = {
        {"clip_gs", sgl::emit::mesh_form::compute, "plain_vs", ""},
        {"clip_gs", sgl::emit::mesh_form::vertex_fetch, "", ""},
        {"clip_gs", sgl::emit::mesh_form::mesh, "plain_vs", ""},
        {"quad_te", sgl::emit::mesh_form::compute, "", "quad_tc"},
        {"quad_te", sgl::emit::mesh_form::vertex_fetch, "", ""},
        {"clip_ms", sgl::emit::mesh_form::compute, "", ""},
        {"clip_ms", sgl::emit::mesh_form::vertex_fetch, "", ""},
    };
    for (auto const t : {target::wgsl, target::hlsl_vulkan, target::msl})
        for (auto const& f : forms)
        {
            auto const e = sgl::compile_to_text({.source = k_stages,
                                                 .entry_point = f.entry,
                                                 .target = t,
                                                 .mesh = f.mesh,
                                                 .polyfill_vertex = f.vertex,
                                                 .polyfill_control = f.control});
            REQUIRE(e.has_error()).dump("entry", f.entry).dump("target", sgl::emit::to_string(t));
            CHECK(e.error().contains("clip distances, which the polyfill does not carry yet"))
                .dump("entry", f.entry)
                .dump("error", e.error());
        }
}

TEST("sgl emit - SGL's own DXIL places each last stage's lanes as the pixel stage after it reads them")
{
    // D3D12 links by register, so the member after the lanes sits on one row on both sides of every link (CHK-574)
    auto const ps = staged("clip_ps", target::hlsl_dx12, native_route::require);
    REQUIRE(ps.has_value()).dump("error", ps.has_error() ? ps.error() : cc::string());
    for (auto const entry : k_last_stages)
    {
        auto const e = staged(entry, target::hlsl_dx12, native_route::require);
        REQUIRE(e.has_value()).dump("entry", entry).dump("error", e.has_error() ? e.error() : cc::string());
        CHECK(!e.value().dxil.empty()).dump("entry", entry);
        auto lanes = 0;
        auto matched = 0;
        for (auto const& out : e.value().signature)
        {
            if (out.table != sgl::ir::dxil::signature_table::output)
                continue;
            // DXC packs the two lanes into one row, as a float2
            if (out.name == "SV_ClipDistance")
            {
                ++lanes;
                CHECK(out.row == 1).dump("entry", entry);
                CHECK(out.first_column == out.semantic_index).dump("entry", entry);
            }
            for (auto const& in : ps.value().signature)
                if (in.table == sgl::ir::dxil::signature_table::input && out.name == in.name
                    && out.semantic_index == in.semantic_index)
                {
                    ++matched;
                    CHECK(out.row == in.row).dump("entry", entry).dump("name", out.name);
                    CHECK(out.first_column == in.first_column).dump("entry", entry).dump("name", out.name);
                }
        }
        CHECK(lanes == 2).dump("entry", entry);
        // the position, the two lanes and the tag
        CHECK(matched == 4).dump("entry", entry);
    }
}

TEST("sgl emit - SGL's own SPIR-V writes each last stage's ClipDistance array, a mesh stage's one per vertex")
{
    for (auto const entry : k_last_stages)
    {
        auto const e = staged(entry, target::hlsl_vulkan, native_route::require);
        REQUIRE(e.has_value()).dump("entry", entry).dump("error", e.has_error() ? e.error() : cc::string());
        auto const& words = e.value().spirv;
        auto capability = false;
        auto clip_variable = u32(0);
        for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        {
            auto const opcode = words[at] & 0xffffu;
            capability = capability || (opcode == 17 && words[at + 1] == 32); // OpCapability ClipDistance
            if (opcode == 71 && words[at + 2] == 11 && words[at + 3] == 3)    // OpDecorate _ BuiltIn ClipDistance
            {
                CHECK(clip_variable == 0u).dump("entry", entry);
                clip_variable = words[at + 1];
            }
        }
        CHECK(capability).dump("entry", entry);
        REQUIRE(clip_variable != 0u).dump("entry", entry);

        // the variable's type, through its pointer: float[2], or float[2][3] for a mesh stage of three vertices
        auto const constant_of = [&](u32 id)
        {
            for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
                if ((words[at] & 0xffffu) == 43 && words[at + 2] == id) // OpConstant
                    return words[at + 3];
            return u32(0);
        };
        auto const defined = [&](u32 id) -> isize
        {
            for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
            {
                auto const opcode = words[at] & 0xffffu;
                auto const result_at = opcode == 59 ? 2 : 1; // OpVariable's result follows its type
                if ((opcode == 59 || opcode == 32 || opcode == 28) && words[at + result_at] == id)
                    return at;
            }
            return -1;
        };
        auto const variable = defined(clip_variable);
        REQUIRE(variable >= 0);
        auto const pointer = defined(words[variable + 1]);
        REQUIRE(pointer >= 0);
        auto const outer = defined(words[pointer + 3]);
        REQUIRE(outer >= 0);
        REQUIRE((words[outer] & 0xffffu) == 28u); // OpTypeArray
        if (cc::string_view(entry) == "clip_ms")
        {
            CHECK(constant_of(words[outer + 3]) == 3u);
            auto const inner = defined(words[outer + 2]);
            REQUIRE(inner >= 0);
            REQUIRE((words[inner] & 0xffffu) == 28u);
            CHECK(constant_of(words[inner + 3]) == 2u);
        }
        else
            CHECK(constant_of(words[outer + 3]) == 2u).dump("entry", entry);
    }
}
TEST("sgl emit - clip distances are each target's own: a lane at a time in HLSL, an array in MSL and WGSL")
{
    // EMIT-290: HLSL a float per lane, which DXC packs as a float4 and the rest and concatenates for vulkan
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const e = compiled("six_vs", t);
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        for (auto lane = 0; lane < 6; ++lane)
            CHECK(e.value().text.contains(cc::format("    float planes_{0} : SV_ClipDistance{0};\n", lane)))
                .dump("text", e.value().text);
        CHECK(e.value().features.has(feature::clip_distances));
    }
    auto const msl = compiled("three_vs", target::msl);
    REQUIRE(msl.has_value()).dump("error", msl.has_error() ? msl.error() : cc::string());
    CHECK(msl.value().text.contains("    float planes [[clip_distance]] [3];\n")).dump("text", msl.value().text);
    CHECK(msl.value().text.contains(".planes[2] = planes[2];")).dump("text", msl.value().text);
    auto const wgsl = compiled("three_vs", target::wgsl);
    REQUIRE(wgsl.has_value()).dump("error", wgsl.has_error() ? wgsl.error() : cc::string());
    CHECK(wgsl.value().text.contains("enable clip_distances;\n")).dump("text", wgsl.value().text);
    CHECK(wgsl.value().text.contains("    @builtin(clip_distances) planes: array<f32, 3>,\n"))
        .dump("text", wgsl.value().text);

    // CHK-574: a link matched by location leaves them out of the pixel stage, so WGSL's fragment input stays valid
    for (auto const t : {target::hlsl_vulkan, target::msl, target::wgsl})
    {
        auto const e = compiled("shade_ps", t);
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        CHECK(!e.value().text.contains("planes")).dump("text", e.value().text);
        CHECK(!e.value().features.has(feature::clip_distances));
    }
    // dx12 matches its stages by register, so its pixel stage declares the lanes the vertex stage writes, unread
    auto const dx12 = compiled("shade_ps", target::hlsl_dx12);
    REQUIRE(dx12.has_value()).dump("error", dx12.has_error() ? dx12.error() : cc::string());
    CHECK(dx12.value().text.contains("    float planes_2 : SV_ClipDistance2;\n")).dump("text", dx12.value().text);
}

TEST("sgl emit - a pixel stage that copies its whole link compiles on every target and route")
{
    // CHK-574: a link matched by location leaves the clip distances out, so a copy of the whole struct names none
    constexpr auto source
        = cc::string_view("require clip_distances\n"
                          "struct varyings:\n"
                          "    @position position: hpos4\n"
                          "    @clip_distances planes: float[2]\n"
                          "    uv: float2\n"
                          "fun pick(a: varyings, b: varyings, c: bool) -> varyings => if c => a else b\n"
                          "@pixel fun ps(p: varyings) -> float4:\n"
                          "    let mut q = p\n"
                          "    q.uv = q.uv * 2.0\n"
                          "    let r = pick(p, q, p.uv.x > 0.5)\n"
                          "    return float4(r.uv.x, q.uv.y, 0.0, 1.0)\n");
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::msl, target::wgsl})
    {
        auto const e = sgl::compile_to_text({.source = source, .entry_point = "ps", .target = t});
        REQUIRE(e.has_value()).dump("target", sgl::emit::to_string(t)).dump("error", e.has_error() ? e.error() : "");
        // dx12 declares them, unread, since it links by register
        if (t != target::hlsl_dx12)
            CHECK(!e.value().text.contains("planes")).dump("text", e.value().text);
    }
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const e
            = sgl::compile_to_text({.source = source, .entry_point = "ps", .target = t, .native = native_route::require});
        REQUIRE(e.has_value()).dump("target", sgl::emit::to_string(t)).dump("error", e.has_error() ? e.error() : "");
    }
}

TEST("sgl emit - a link's member after the clip distances sits on the same DXIL row on both sides of the link")
{
    // D3D12 links a vertex and a pixel stage by register, so every input of the pixel stage must sit where the
    // vertex stage wrote it, which is why the pixel stage declares the clip distances there, unread (CHK-574).
    auto const vs = compiled("three_vs", target::hlsl_dx12, native_route::prefer);
    auto const ps = compiled("shade_ps", target::hlsl_dx12, native_route::prefer);
    REQUIRE(vs.has_value()).dump("error", vs.has_error() ? vs.error() : cc::string());
    REQUIRE(ps.has_value()).dump("error", ps.has_error() ? ps.error() : cc::string());
    auto matched = 0;
    for (auto const& out : vs.value().signature)
        for (auto const& in : ps.value().signature)
            if (out.table == sgl::ir::dxil::signature_table::output && in.table == sgl::ir::dxil::signature_table::input
                && out.name == in.name && out.semantic_index == in.semantic_index)
            {
                ++matched;
                CHECK(out.row == in.row).dump("name", out.name).dump("vertex row", out.row).dump("pixel row", in.row);
                CHECK(out.first_column == in.first_column).dump("name", out.name);
            }
    // the position, the three lanes and the uv
    CHECK(matched == 5);
}

TEST("sgl emit - SGL's own SPIR-V writes one ClipDistance array of the lanes, and its own DXIL a lane per element")
{
    for (auto const [entry, lanes] : {cc::pair<cc::string_view, u32>{"three_vs", 3u}, {"six_vs", 6u}})
    {
        auto const vulkan = compiled(entry, target::hlsl_vulkan, native_route::require);
        REQUIRE(vulkan.has_value()).dump("error", vulkan.has_error() ? vulkan.error() : cc::string());
        auto const& words = vulkan.value().spirv;
        auto capability = false;
        auto built_in_variables = 0;
        auto array_of_lanes = false;
        for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        {
            auto const opcode = words[at] & 0xffffu;
            capability = capability || (opcode == 17 && words[at + 1] == 32); // OpCapability ClipDistance
            // OpDecorate _ BuiltIn ClipDistance
            built_in_variables += opcode == 71 && words[at + 2] == 11 && words[at + 3] == 3 ? 1 : 0;
            // OpTypeArray _ _ <constant>: the constant's value is checked by the count alone
            array_of_lanes = array_of_lanes || opcode == 28;
        }
        CHECK(capability).dump("entry", entry);
        CHECK(built_in_variables == 1).dump("entry", entry);
        CHECK(array_of_lanes).dump("entry", entry);
        (void)lanes;

        auto const dx12 = compiled(entry, target::hlsl_dx12, native_route::require);
        REQUIRE(dx12.has_value()).dump("error", dx12.has_error() ? dx12.error() : cc::string());
        CHECK(!dx12.value().dxil.empty()).dump("entry", entry);
    }
}

TEST("sgl check - a task stage's payload reaches no rasterizer, so it holds no clip distances")
{
    // CHK-571: what reaches no rasterizer is invalid-entry-point, a task stage's payload among it, whether or not a
    // mesh stage takes it
    auto const payload = reports_for(R"(require clip_distances, mesh_shader
struct handed:
    @clip_distances planes: float[1]
@task(1) fun ts():
    let h: handed = {planes = [1.0]}
    dispatch_mesh(1u, 1u, 1u, h)
)");
    CHECK(payload.contains("invalid-entry-point")).dump("report", payload);
    CHECK(payload.contains("'planes'")).dump("report", payload);
}

TEST("sgl check - a pixel stage takes the clip distances as the stage before it hands them, attribute and all")
{
    // CHK-183, CHK-574: the stages pass one interface member for member; a pixel stage that takes the lanes as an
    // ordinary member would read them by location where dx12 links them by register as SV_ClipDistance
    auto const unmarked = reports_for(stages_with("@pixel fun clip_ps(p: clipped) -> float4 => float4(p.tag, 0.0, 0.0, "
                                                  "1.0)\n",
                                                  "struct taken:\n"
                                                  "    @position position: hpos4\n"
                                                  "    planes: float[2]\n"
                                                  "    tag: float\n"
                                                  "@pixel fun clip_ps(p: taken) -> float4 => float4(p.tag, 0.0, 0.0, "
                                                  "1.0)\n"));
    CHECK(unmarked.contains("invalid-pipeline")).dump("report", unmarked);
    // and the other way: a last stage that writes plain values, and a pixel stage that takes them as clip distances
    auto const marked = reports_for(R"(require clip_distances
struct written:
    @position position: hpos4
    planes: float[2]
struct taken:
    @position position: hpos4
    @clip_distances planes: float[2]
@vertex fun vs(@vertex_index i: int) -> written:
    return {position = hpos4(0.0, 0.0, 0.0, 1.0), planes = float[2].filled(1.0)}
@pixel fun ps(p: taken) -> float4 => float4(1.0)
pipeline p:
    vertex = vs
    pixel = ps
    format = .rgba8_unorm
)");
    CHECK(marked.contains("invalid-pipeline")).dump("report", marked);
}

TEST("sgl check - a @per_draw clip distance of a geometry or mesh stage is reported once")
{
    auto const per_draw = reports_for(stages_with("@clip_distances planes", "@clip_distances @per_draw planes"));
    // one report for each of clip_gs, clip_ms and quad_te, CHK-393's, and not a second one of CHK-571's for the field
    for (auto const entry : k_last_stages)
    {
        auto reports = 0;
        for (auto at = per_draw.find(cc::format("user:[{}]", entry)); at >= 0;
             at = per_draw.find(cc::format("user:[{}]", entry), at + 1))
            ++reports;
        CHECK(reports == 1).dump("entry", entry).dump("report", per_draw);
    }
    CHECK(per_draw.contains("only a vertex stage writes one yet")).dump("report", per_draw);
    CHECK(!per_draw.contains("user:[clip_gs] the @clip_distances member")).dump("report", per_draw);
}

TEST("sgl emit - a mesh stage's clip distances after another member, beside values per triangle, on SGL's own routes")
{
    // EMIT-290: the lanes are a vertex's however the struct orders them, and its triangle's struct takes none
    constexpr auto program = cc::string_view(R"(require clip_distances, mesh_shader
struct one_clip:
    @position position: hpos4
    tag: float
    @clip_distances planes: float[1]
struct face:
    @interpolate(.flat) id: uint
@mesh(1, vertices = 3, triangles = 1) fun ms(output: mut mesh_output[one_clip, face]):
    output.set_counts(3u, 1u)
    for t in 0 ..< 3:
        output.vertex(uint(t), {position = hpos4(-1.0, -1.0, 0.0, 1.0), tag = 1.0, planes = [float(t)]})
    output.triangle(0u, uint3(0u, 1u, 2u))
    output.primitive(0u, {id = 7u})
@pixel fun ps(p: one_clip, f: face) -> float4 => float4(p.tag, float(f.id), 0.0, 1.0)
pipeline meshed:
    mesh = ms
    pixel = ps
    format = .rgba32_float
)");
    auto const clean = reports_for(program);
    CHECK(clean == "").dump("report", clean);
    auto const spirv = sgl::compile_to_text(
        {.source = program, .entry_point = "ms", .target = target::hlsl_vulkan, .native = native_route::require});
    REQUIRE(spirv.has_value()).dump("error", spirv.has_error() ? spirv.error() : cc::string());
    // the one lane is written at [vertex][0]: an OpAccessChain into the ClipDistance variable with two indices
    auto const& words = spirv.value().spirv;
    auto clip_variable = u32(0);
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 71 && words[at + 2] == 11 && words[at + 3] == 3)
            clip_variable = words[at + 1];
    REQUIRE(clip_variable != 0u);
    auto chains = 0;
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 65 && words[at + 3] == clip_variable) // OpAccessChain _ _ base indices...
        {
            ++chains;
            CHECK((words[at] >> 16) == 6u); // two indices: the vertex and the lane
        }
    CHECK(chains >= 1);
    for (auto const entry : {"ms", "ps"})
    {
        auto const dxil = sgl::compile_to_text(
            {.source = program, .entry_point = entry, .target = target::hlsl_dx12, .native = native_route::require});
        REQUIRE(dxil.has_value()).dump("entry", entry).dump("error", dxil.has_error() ? dxil.error() : cc::string());
    }
    // every polyfill form of the mesh stage refuses them, the scan and the launched one too
    for (auto const form : {sgl::emit::mesh_form::scan, sgl::emit::mesh_form::launched})
    {
        auto const e
            = sgl::compile_to_text({.source = k_stages, .entry_point = "clip_ms", .target = target::wgsl, .mesh = form});
        REQUIRE(e.has_error());
        CHECK(e.error().contains("clip distances, which the polyfill does not carry yet")).dump("error", e.error());
    }
}
