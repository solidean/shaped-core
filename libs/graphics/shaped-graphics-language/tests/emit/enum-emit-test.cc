#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

namespace
{
/// An enum, a local of it and a comparison, over the edge structs every emit test shares.
constexpr cc::string_view k_source = "enum light_kind:\n"
                                     "    point\n"
                                     "    spot\n"
                                     "    sun\n"
                                     "\n"
                                     "@pixel struct frame:\n"
                                     "    color: float4\n"
                                     "\n"
                                     "struct pixel_input:\n"
                                     "    @position position: hpos4\n"
                                     "    normal: vec3\n"
                                     "\n"
                                     "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                     "    let kind = light_kind.spot\n"
                                     "    let is_sun = kind == light_kind.sun\n"
                                     "    if is_sun => return {color = float4(1.0, 1.0, 1.0, 1.0)}\n"
                                     "    return {color = float4(0.0, 0.0, 0.0, 1.0)}\n";

cc::string text_of(target t)
{
    auto const e = emit_source(k_source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

TEST("sgl emit - an enum is one named int constant per case, in front of the structs")
{
    auto const hlsl = text_of(target::hlsl_dx12);
    CHECK(hlsl.contains("static const int light_kind_point = 0;\n"));
    CHECK(hlsl.contains("static const int light_kind_spot = 1;\n"));
    CHECK(hlsl.contains("static const int light_kind_sun = 2;\n"));

    auto const wgsl = text_of(target::wgsl);
    CHECK(wgsl.contains("const light_kind_point: i32 = 0;\n"));
    CHECK(wgsl.contains("const light_kind_sun: i32 = 2;\n"));

    auto const msl = text_of(target::msl);
    CHECK(msl.contains("constant int light_kind_point = 0;\n"));
    // MSL's include comes first, or the `constant` address space has met no declaration of it (EMIT-56).
    CHECK(msl.find(cc::string_view("#include <metal_stdlib>")) < msl.find(cc::string_view("constant int "
                                                                                          "light_kind_point")));

    // Every case is written, whether or not the entry point names it: `point` is in none of the text above it (EMIT-77).
    CHECK(hlsl.contains("light_kind_point"));
    CHECK(!hlsl.contains("kind == light_kind_point"));
}

TEST("sgl emit - an enum value is its constant and a comparison is the target's ==")
{
    auto const hlsl = text_of(target::hlsl_dx12);
    CHECK(hlsl.contains("const int kind = light_kind_spot;"));
    CHECK(hlsl.contains("const bool is_sun = kind == light_kind_sun;"));

    auto const wgsl = text_of(target::wgsl);
    CHECK(wgsl.contains("let kind: i32 = light_kind_spot;"));
    CHECK(wgsl.contains("let is_sun: bool = kind == light_kind_sun;"));

    auto const msl = text_of(target::msl);
    CHECK(msl.contains("const int kind = light_kind_spot;"));

    // The two HLSL targets differ in their addresses alone, never in a statement.
    CHECK(text_of(target::hlsl_vulkan).contains("const bool is_sun = kind == light_kind_sun;"));
}

TEST("sgl emit - an enum crossing an edge or riding in a binding is unsupported")
{
    // EMIT-33 and EMIT-39 ask for a builtin record, and an enum has none; the answer matches `int` and `bool`.
    auto const in_link = "enum k:\n    a\n\n@pixel struct frame:\n    color: float4\n\n"
                         "struct pixel_input:\n    @position position: hpos4\n    kind: k\n\n"
                         "@pixel fun main_ps(p: pixel_input) -> frame => {color = float4(0.0, 0.0, 0.0, 1.0)}\n";
    auto const e = emit_source(in_link, 0, target::hlsl_dx12);
    CHECK(e.text == "");
    CHECK(sgl::emit::dump_errors(e).contains("unsupported"));
}
