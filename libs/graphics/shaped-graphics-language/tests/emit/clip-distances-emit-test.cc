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

TEST("sgl check - only what reaches the rasterizer clips, and a geometry or tessellation stage writes none yet")
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
        out.append({position = v[i].position})
@pixel fun ps(p: gout) -> float4 => float4(1.0)
pipeline p:
    vertex = vs
    geometry = gs
    pixel = ps
    format = .rgba8_unorm
)");
    CHECK(handed.contains("invalid-pipeline")).dump("report", handed);
    CHECK(handed.contains("only what reaches the rasterizer clips")).dump("report", handed);
    // CHK-571: a geometry stage that writes them is no form the compiler carries yet
    auto const geometry = reports_for(R"(require clip_distances, geometry_shader
struct vin:
    @position position: hpos4
struct gout:
    @position position: hpos4
    @clip_distances planes: float[2]
@geometry(max_vertices = 3) fun gs(v: vin[3], out: mut triangle_stream[gout]):
    for i in 0 ..< 3:
        out.append({position = v[i].position, planes = float[2].filled(1.0)})
)");
    CHECK(geometry.contains("unsupported-yet")).dump("report", geometry);
    CHECK(geometry.contains("clip distances in a geometry stage")).dump("report", geometry);
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

    // CHK-574: the pixel stage's link leaves them out on every target, so WGSL's fragment input stays valid
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::msl, target::wgsl})
    {
        auto const e = compiled("shade_ps", t);
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        CHECK(!e.value().text.contains("planes")).dump("text", e.value().text);
        CHECK(!e.value().features.has(feature::clip_distances));
    }
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
