#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

// The maths of a double, which HLSL writes as functions of its own text (CHK-458).

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

TEST("sgl emit - a double's maths is the text's own HLSL, declared once whatever calls it")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const e = emit_source(k_rounded, 0, t);
        CHECK(sgl::emit::dump_errors(e) == "").dump("target", sgl::emit::to_string(t));
        auto const& hlsl = e.text;
        // DXC narrows a double handed to its own sqrt or floor to a float, so neither is called
        CHECK(hlsl.contains("sgl_double_sqrt(v.x)"));
        CHECK(hlsl.contains("sgl_double_floor(v.y)"));
        CHECK(hlsl.contains("sgl_double_round(v.z)"));
        CHECK(hlsl.contains("sgl_double_length(v.xy)"));
        CHECK(hlsl.contains("sgl_double_normalize(v)"));
        CHECK(hlsl.contains("sgl_double_mix(v, r, 0.5"));
        // HLSL's sign of a double is an int, and converts back
        CHECK(hlsl.contains("double(sign(v.w))"));
        // one text for all of them, declared once
        auto const declaration = cc::string_view("double sgl_double_trunc(double x)\n");
        auto const first = hlsl.find(declaration);
        CHECK(first >= 0);
        CHECK(hlsl.subview(first + declaration.size()).find(declaration) < 0);
        CHECK(!hlsl.contains(" sqrt("));
        CHECK(!hlsl.contains(" floor("));
    }
}

TEST("sgl emit - no target but HLSL is handed a double's maths")
{
    // EMIT-109 and EMIT-192: WGSL has no double, and MSL refuses one
    for (auto const t : {target::wgsl, target::msl})
        CHECK(sgl::emit::dump_errors(emit_source(k_rounded, 0, t)) != "").dump("target", sgl::emit::to_string(t));
}
