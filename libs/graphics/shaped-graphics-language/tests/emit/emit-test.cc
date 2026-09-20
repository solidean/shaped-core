#include "emit-test-support.hh"

#include <shaped-graphics-language/emit/reserved_words.hh>

using namespace sgl_test;
using sgl::emit::target;

namespace
{
/// The edge structs most programs below share; `frame` and `pixel_input` are reserved nowhere.
constexpr cc::string_view k_edges = "@pixel struct frame:\n"
                                    "    color: float4\n"
                                    "\n"
                                    "struct pixel_input:\n"
                                    "    @position position: hpos4\n"
                                    "    normal: vec3\n"
                                    "\n";

cc::string with_edges(cc::string_view rest)
{
    return cc::string(k_edges) + rest;
}

/// The text of entry point 0, which must exist.
cc::string text_of(cc::string_view user, target t)
{
    auto const e = emit_source(user, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}

/// The errors of entry point 0, which must be the same for every target.
cc::string errors_of(cc::string_view user)
{
    auto const first = emit_source(user, 0, target::hlsl_dx12);
    CHECK(first.text == "");
    for (auto const t : sgl::emit::all_targets())
        CHECK(emit_source(user, 0, t) == first);
    return sgl::emit::dump_errors(first);
}
} // namespace

TEST("sgl emit - a local that is reserved in one target gets its underscore there and nowhere else")
{
    auto const source = with_edges("@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   "    let filter = normalize p.normal\n"
                                   "    let mul = saturate dot(filter, filter)\n"
                                   "    return {\n"
                                   "        color = float4(..filter, mul)\n"
                                   "    }\n");

    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(hlsl.contains("    const float3 filter = normalize(p.normal);\n"));
    CHECK(hlsl.contains("    const float mul_ = saturate(dot(filter, filter));\n"));
    CHECK(hlsl.contains("    result.color = float4(filter.x, filter.y, filter.z, mul_);\n"));

    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("    let filter_: vec3f = normalize(p.normal);\n"));
    CHECK(wgsl.contains("    let mul: f32 = saturate(dot(filter_, filter_));\n"));
    CHECK(wgsl.contains("    return frame(vec4f(filter_.x, filter_.y, filter_.z, mul));\n"));

    // `filter` is an enum of the metal namespace, and `mul` is nothing there
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("    const float3 filter_ = normalize(p.normal);\n"));
    CHECK(msl.contains("    const float mul = saturate(dot(filter_, filter_));\n"));
    CHECK(msl.contains("    result.color = float4(filter_.x, filter_.y, filter_.z, mul);\n"));
}

TEST("sgl emit - a name that only MSL reserves is renamed in MSL and nowhere else")
{
    auto const source = with_edges("struct device:\n"
                                   "    fract: float\n"
                                   "\n"
                                   "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   "    let kernel = device(0.5)\n"
                                   "    return {\n"
                                   "        color = float4(kernel.fract, kernel.fract, kernel.fract, 1.0)\n"
                                   "    }\n");

    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("struct device_\n{\n    float fract_;\n};\n"));
    CHECK(msl.contains("    device_ kernel_;\n    kernel_.fract_ = 0.5;\n"));
    CHECK(msl.contains("float4(kernel_.fract_, kernel_.fract_, kernel_.fract_, 1.0);\n"));

    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("struct device {\n    fract: f32,\n}\n"));
    CHECK(wgsl.contains("    let kernel: device = device(0.5);\n"));
}

TEST("sgl emit - a reserved member is renamed, and its dx12 semantic still comes from the name as written")
{
    auto const source = cc::string_view("@inline binding constants:\n"
                                        "    matrix: mat4\n"
                                        "\n"
                                        "@vertex struct mesh_vertex:\n"
                                        "    point: pos3\n"
                                        "\n"
                                        "struct pixel_input:\n"
                                        "    @position position: hpos4\n"
                                        "\n"
                                        "@vertex fun main_vs(v: mesh_vertex){constants} -> pixel_input:\n"
                                        "    return {\n"
                                        "        position = constants.matrix * v.point\n"
                                        "    }\n");

    auto const hlsl = text_of(source, target::hlsl_vulkan);
    CHECK(hlsl.contains("    [[vk::location(0)]] float3 point_ : POINT;\n"));
    CHECK(hlsl.contains("    [[vk::offset(0)]] column_major float4x4 matrix_;\n"));
    CHECK(hlsl.contains("    result.position = mul(constants.matrix_, float4(v.point_, 1.0));\n"));

    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("    @location(0) point: vec3f,\n"));
    CHECK(wgsl.contains("    return pixel_input(constants.matrix * vec4f(v.point, 1.0));\n"));
}

TEST("sgl emit - an underscore that is taken already is minted past")
{
    auto const source = with_edges("struct target_:\n"
                                   "    x: float\n"
                                   "\n"
                                   "@pixel struct target:\n"
                                   "    color: float4\n"
                                   "\n"
                                   "@pixel fun main_ps(p: pixel_input) -> target:\n"
                                   "    let t = target_(0.5)\n"
                                   "    return {\n"
                                   "        color = float4(..p.normal, t.x)\n"
                                   "    }\n");
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("struct target_ {\n    x: f32,\n}\n"));
    CHECK(wgsl.contains("struct target__1 {\n    @location(0) color: vec4f,\n}\n"));
    CHECK(wgsl.contains("fn main_ps(p: pixel_input) -> target__1 {\n"));
}

TEST("sgl emit - an entry point name that is reserved in any target is an error in every target")
{
    CHECK(errors_of(with_edges("@pixel fun filter(p: pixel_input) -> frame:\n"
                               "    return {\n"
                               "        color = float4(..p.normal, 1.0)\n"
                               "    }\n"))
          == "reserved-entry-point-name 'filter' is reserved in wgsl, msl\n");

    CHECK(errors_of(with_edges("@pixel fun mul(p: pixel_input) -> frame:\n"
                               "    return {\n"
                               "        color = float4(..p.normal, 1.0)\n"
                               "    }\n"))
          == "reserved-entry-point-name 'mul' is reserved in hlsl-dx12, hlsl-vulkan\n");

    // MSL forbids a function called `main`, so no target gets one
    CHECK(errors_of(with_edges("@pixel fun main(p: pixel_input) -> frame:\n"
                               "    return {\n"
                               "        color = float4(..p.normal, 1.0)\n"
                               "    }\n"))
          == "reserved-entry-point-name 'main' is reserved in msl\n");

    // and the names the cube uses stay free everywhere
    for (auto const t : sgl::emit::all_targets())
    {
        CHECK(!sgl::emit::is_reserved(t, "main_vs"));
        CHECK(!sgl::emit::is_reserved(t, "main_ps"));
    }
}

TEST("sgl emit - a binding that is not @inline is an error and never a guessed address")
{
    auto const source = with_edges("binding scene:\n"
                                   "    tint: float3\n"
                                   "\n"
                                   "@pixel fun main_ps(p: pixel_input){scene} -> frame:\n"
                                   "    return {\n"
                                   "        color = float4(..scene.tint, 1.0)\n"
                                   "    }\n");
    CHECK(errors_of(source) == "unsupported a binding that is not @inline: 'scene'\n");

    auto const e = emit_source(source, 0, target::wgsl);
    REQUIRE(e.errors.size() == 1);
    CHECK(e.errors[0].kind == sgl::emit::error_kind::unsupported);
    CHECK(sgl::check::is_valid(e.errors[0].symbol));
}

TEST("sgl emit - an inline block whose members would sit elsewhere in one target than in another is an error")
{
    CHECK(errors_of(with_edges("@inline binding look:\n"
                               "    scale: float\n"
                               "    tint: float3\n"
                               "\n"
                               "@pixel fun main_ps(p: pixel_input){look} -> frame:\n"
                               "    return {\n"
                               "        color = float4(..look.tint, look.scale)\n"
                               "    }\n"))
          == "layout-mismatch 'look.tint' is at byte 4 in HLSL, at byte 16 in WGSL and at byte 16 in MSL\n");

    // MSL alone disagrees here: its float3 is 16 bytes, so nothing fits into the tail HLSL and WGSL both fill.
    CHECK(errors_of(with_edges("@inline binding look:\n"
                               "    tint: float3\n"
                               "    scale: float\n"
                               "\n"
                               "@pixel fun main_ps(p: pixel_input){look} -> frame:\n"
                               "    return {\n"
                               "        color = float4(..look.tint, look.scale)\n"
                               "    }\n"))
          == "layout-mismatch 'look.scale' is at byte 12 in HLSL, at byte 12 in WGSL and at byte 16 in MSL\n");

    // A block of full rows packs alike everywhere, and vulkan states each offset since a push-constant block packs tight.
    auto const hlsl = text_of(with_edges("@inline binding look:\n"
                                         "    tint: float3\n"
                                         "    to_world: mat4\n"
                                         "    scale: float\n"
                                         "\n"
                                         "@pixel fun main_ps(p: pixel_input){look} -> frame:\n"
                                         "    let n = look.to_world * p.normal\n"
                                         "    return {\n"
                                         "        color = float4(..n, look.scale)\n"
                                         "    }\n"),
                              target::hlsl_vulkan);
    CHECK(hlsl.contains("    [[vk::offset(0)]] float3 tint;\n"));
    CHECK(hlsl.contains("    [[vk::offset(16)]] column_major float4x4 to_world;\n"));
    CHECK(hlsl.contains("    [[vk::offset(80)]] float scale;\n"));
    CHECK(hlsl.contains("    const float3 n = mul(look.to_world, float4(p.normal, 0.0)).xyz;\n"));
}

TEST("sgl emit - a direction is transformed with w = 0, and WGSL parenthesizes the product it swizzles")
{
    auto const wgsl = text_of(with_edges("@inline binding look:\n"
                                         "    to_world: mat4\n"
                                         "\n"
                                         "@pixel fun main_ps(p: pixel_input){look} -> frame:\n"
                                         "    let n = look.to_world * p.normal\n"
                                         "    return {\n"
                                         "        color = float4(..n, 1.0)\n"
                                         "    }\n"),
                              target::wgsl);
    CHECK(wgsl.contains("    let n: vec3f = (look.to_world * vec4f(p.normal, 0.0)).xyz;\n"));
}

TEST("sgl emit - a float literal has a decimal point and reads back as the same value")
{
    auto const source = with_edges("@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   "    let a = saturate 3.0\n"
                                   "    let b = saturate 1e20\n"
                                   "    let c = saturate 2.5e-7\n"
                                   "    let d = saturate 0.1\n"
                                   "    let e = saturate 16777217.0\n"
                                   "    return {\n"
                                   "        color = float4(a, b, c, d * e)\n"
                                   "    }\n");
    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(hlsl.contains("a = saturate(3.0);\n"));
    CHECK(hlsl.contains("b = saturate(1.0e+20);\n"));
    CHECK(hlsl.contains("c = saturate(2.5e-07);\n"));
    CHECK(hlsl.contains("d = saturate(0.1);\n"));
    CHECK(hlsl.contains("e = saturate(16777217.0);\n"));

    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("b: f32 = saturate(1.0e+20);\n"));
    CHECK(wgsl.contains("c: f32 = saturate(2.5e-07);\n"));
}

TEST("sgl emit - parentheses follow the tree, and a negative literal needs none")
{
    auto const source = with_edges("@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   "    let a = saturate 0.5\n"
                                   "    let b = (a + a) * a\n"
                                   "    let c = a + (a + b)\n"
                                   "    let d = a * b + c * -2.5\n"
                                   "    let e = a * (b * c)\n"
                                   "    return {\n"
                                   "        color = float4(b, c, d, e)\n"
                                   "    }\n");
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("    let b: f32 = (a + a) * a;\n"));
    CHECK(wgsl.contains("    let c: f32 = a + (a + b);\n"));
    CHECK(wgsl.contains("    let d: f32 = a * b + c * -2.5;\n"));
    CHECK(wgsl.contains("    let e: f32 = a * (b * c);\n"));
}

TEST("sgl emit - a struct of the program is declared before its first use, and built the way the target can")
{
    auto const source = with_edges("struct light:\n"
                                   "    towards: vec3\n"
                                   "    power: float\n"
                                   "\n"
                                   "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                   "    let sun = light(normalize p.normal, 0.5)\n"
                                   "    return {\n"
                                   "        color = float4(..sun.towards, sun.power)\n"
                                   "    }\n");

    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(hlsl.contains("struct light\n{\n    float3 towards;\n    float power;\n};\n"));
    CHECK(hlsl.contains("    light sun;\n"
                        "    sun.towards = normalize(p.normal);\n"
                        "    sun.power = 0.5;\n"));
    // a splatted value that is no local was bound to a temporary by the check pass, and it reads like any `let`
    CHECK(hlsl.contains("    const float3 splat = sun.towards;\n"));
    CHECK(hlsl.contains("    result.color = float4(splat.x, splat.y, splat.z, sun.power);\n"));

    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("struct light {\n    towards: vec3f,\n    power: f32,\n}\n"));
    CHECK(wgsl.contains("    let sun: light = light(\n"
                        "        normalize(p.normal),\n"
                        "        0.5\n"
                        "    );\n"));
}

TEST("sgl emit - a member whose dx12 semantic would be a system value is an error")
{
    CHECK(errors_of("@vertex struct mesh_vertex:\n"
                    "    sv_position: pos3\n"
                    "\n"
                    "struct pixel_input:\n"
                    "    @position position: hpos4\n"
                    "\n"
                    "@inline binding constants:\n"
                    "    view_projection: mat4\n"
                    "\n"
                    "@vertex fun main_vs(v: mesh_vertex){constants} -> pixel_input:\n"
                    "    return {\n"
                    "        position = constants.view_projection * v.sv_position\n"
                    "    }\n")
          == "system-value-semantic 'mesh_vertex.sv_position'\n");
}

TEST("sgl emit - emit is total: a module with errors and a position out of range are errors in the result")
{
    auto const broken = check_sources(read_prelude(), with_edges("@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                                                 "    return {\n"
                                                                 "        color = nothing\n"
                                                                 "    }\n"));
    CHECK(sgl::emit::dump_errors(sgl::emit::emit(broken.module, 0, target::wgsl)) == "module-has-errors 1 errors\n");

    auto const cube = check_sources(read_prelude(), read_text(cc::string(SGL_SAMPLES_DIR) + "/cube.sgl"));
    CHECK(sgl::emit::dump_errors(sgl::emit::emit(cube.module, 2, target::wgsl)) == "unknown-entry-point 2 of 2\n");
    CHECK(sgl::emit::dump_errors(sgl::emit::emit(cube.module, -1, target::hlsl_dx12)) == "unknown-entry-point -1 of 2\n");
}

TEST("sgl emit - the reserved words differ by target and hold no word twice")
{
    CHECK(sgl::emit::is_reserved(target::wgsl, "target"));
    CHECK(sgl::emit::is_reserved(target::wgsl, "filter"));
    CHECK(!sgl::emit::is_reserved(target::hlsl_dx12, "target"));
    CHECK(sgl::emit::is_reserved(target::hlsl_vulkan, "float4x4"));
    CHECK(sgl::emit::is_reserved(target::hlsl_dx12, "mul"));
    CHECK(!sgl::emit::is_reserved(target::wgsl, "mul"));
    CHECK(sgl::emit::is_reserved(target::msl, "main"));
    CHECK(sgl::emit::is_reserved(target::msl, "fragment"));
    CHECK(sgl::emit::is_reserved(target::msl, "packed_float3"));
    CHECK(!sgl::emit::is_reserved(target::msl, "target"));
    CHECK(!sgl::emit::is_reserved(target::hlsl_dx12, "main"));
    // what the hand-written cube shaders call their locals stays free
    CHECK(!sgl::emit::is_reserved(target::hlsl_dx12, "lit"));
    CHECK(!sgl::emit::is_reserved(target::wgsl, "input"));
    CHECK(!sgl::emit::is_reserved(target::msl, "lit"));

    for (auto const t : sgl::emit::all_targets())
    {
        auto const words = sgl::emit::reserved_words(t);
        auto duplicates = cc::string();
        for (auto i = isize(0); i < words.size(); ++i)
            for (auto j = i + 1; j < words.size(); ++j)
                if (words[i] == words[j])
                    duplicates.appendf("{} ", words[i]);
        CHECK(duplicates == "");
    }
}
