#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::emit::target;

// The maths of a double: dx12 writes core.sgl's bodies over its bits and `fma`, so HLSL calls none of its own (EMIT-219),
// and vulkan writes GLSL.std.450's own through DXC's inline SPIR-V (EMIT-256).

namespace
{
constexpr cc::string_view k_rounded = "require shader_f64\n"
                                      "\n"
                                      "binding work:\n"
                                      "    values: mut buffer[double4]\n"
                                      "\n"
                                      "@compute(64) fun run(@thread_id id: int3){work}:\n"
                                      "    let v = work.values[id.x]\n"
                                      "    let r = double4(sqrt(v.x), floor(v.y), round(v.z), length(v.xy))\n"
                                      "    work.values[id.x] = r + normalize(v) * sign(v.w) + mix(v, r, 0.5f64)\n";
} // namespace

TEST("sgl emit - a double's maths on dx12 is SGL inlined into the text, over its bits and fma alone")
{
    {
        auto const t = target::hlsl_dx12;
        // compiled for the target, as a shader is: core.sgl branches on the API it is written for (CHK-426)
        auto const e = sgl::compile_to_text({.source = k_rounded, .entry_point = "run", .target = t});
        REQUIRE(e.has_value());
        auto const& hlsl = e.value().text;
        // DXC narrows a double handed to its own sqrt, floor, round, length or lerp to a float, so none is called
        for (auto const own : {"sqrt(", "floor(", "round(", "trunc(", "length(", "normalize(", "lerp(", "frac("})
        {
            CHECK(!hlsl.contains(cc::format(" {}", own))).dump("call", own).dump("target", sgl::emit::to_string(t));
            CHECK(!hlsl.contains(cc::format("({}", own))).dump("call", own).dump("target", sgl::emit::to_string(t));
        }
        // what core.sgl writes them over: the words of a double, HLSL's double `fma`, and the float's rsqrt it refines
        CHECK(hlsl.contains("sgl_double_bits("));
        CHECK(hlsl.contains("sgl_double_from_bits("));
        CHECK(hlsl.contains("fma("));
        CHECK(hlsl.contains("rsqrt("));
        // HLSL's sign of a double is an int, and converts back
        CHECK(hlsl.contains("double(sign(v.w))"));
        // each helper of the text is declared once, however many calls inline it
        auto const declaration = cc::string_view("uint2 sgl_double_bits(double x)\n");
        auto const first = hlsl.find(declaration);
        CHECK(first >= 0);
        CHECK(hlsl.subview(first + declaration.size()).find(declaration) < 0);
        CHECK(!hlsl.contains("sgl_double_sqrt"));
        CHECK(!hlsl.contains("ext_instruction"));
    }
}

TEST("sgl emit - a double's maths on vulkan is GLSL.std.450's, declared through DXC's inline SPIR-V")
{
    auto const e = sgl::compile_to_text({.source = k_rounded, .entry_point = "run", .target = target::hlsl_vulkan});
    REQUIRE(e.has_value());
    auto const& hlsl = e.value().text;
    // DXC's own would take a float, so each is a declared instruction, and none of dx12's bodies is written
    CHECK(hlsl.contains("[[vk::ext_instruction(/* Sqrt */ 31, \"GLSL.std.450\")]] double sgl_spirv_sqrt(double x);"));
    CHECK(hlsl.contains("[[vk::ext_instruction(/* Floor */ 8, \"GLSL.std.450\")]] double sgl_spirv_floor(double x);"));
    CHECK(hlsl.contains("[[vk::ext_instruction(/* RoundEven */ 2, \"GLSL.std.450\")]] double sgl_spirv_round(double "
                        "x);"));
    CHECK(hlsl.contains("sgl_spirv_sqrt("));
    CHECK(!hlsl.contains("rsqrt("));
    for (auto const own : {"sqrt(", "floor(", "round(", "frac("})
    {
        CHECK(!hlsl.contains(cc::format(" {}", own))).dump("call", own);
        CHECK(!hlsl.contains(cc::format("({}", own))).dump("call", own);
    }
    // each declaration once, however many calls take it: `length` and `normalize` reach the root and its inverse again
    auto const declaration = cc::string_view("double sgl_spirv_sqrt(double x);\n");
    auto const first = hlsl.find(declaration);
    CHECK(first >= 0);
    CHECK(hlsl.subview(first + declaration.size()).find(declaration) < 0);
}

TEST("sgl emit - no target but HLSL is handed a double's maths")
{
    // EMIT-109 and EMIT-192: WGSL has no double, and MSL refuses one
    for (auto const t : {target::wgsl, target::msl})
        CHECK(sgl::emit::dump_errors(emit_source(k_rounded, 0, t)) != "").dump("target", sgl::emit::to_string(t));
}
