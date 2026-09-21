#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

namespace
{
constexpr cc::string_view k_edges = "@pixel struct frame:\n"
                                    "    color: float4\n"
                                    "\n"
                                    "struct pixel_input:\n"
                                    "    @position position: hpos4\n"
                                    "    normal: vec3\n"
                                    "\n";

cc::string text_of(cc::string_view rest, target t)
{
    auto const e = emit_source(cc::string(k_edges) + rest, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}

/// An enum, and a `case` over it whose value the entry point reads.
constexpr cc::string_view k_over_enum = "enum light_kind:\n"
                                        "    point\n"
                                        "    spot\n"
                                        "    sun\n"
                                        "\n"
                                        "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                        "    let kind = light_kind.spot\n"
                                        "    let w = case kind:\n"
                                        "        .point => 1.0\n"
                                        "        .spot => 0.5\n"
                                        "        .sun => 0.25\n"
                                        "    return {color = float4(w, w, w, 1.0)}\n";
} // namespace

TEST("sgl emit - a case over an enum is a switch whose labels are the constants")
{
    auto const wgsl = text_of(k_over_enum, target::wgsl);
    CHECK(wgsl.contains("    switch kind {\n"));
    CHECK(wgsl.contains("        case light_kind_point: {\n"));
    // The last arm is the default, so no target writes an arm nobody could reach (CHK-159).
    CHECK(wgsl.contains("        default: {\n"));
    CHECK(!wgsl.contains("case light_kind_sun:"));

    auto const hlsl = text_of(k_over_enum, target::hlsl_dx12);
    CHECK(hlsl.contains("    switch (kind)\n"));
    CHECK(hlsl.contains("    case light_kind_point:\n"));
    // The C-like targets fall through without it, and the emitter is what writes it (EMIT-79).
    CHECK(hlsl.contains("        break;\n"));
}

TEST("sgl emit - a case that costs one var and no flag")
{
    // LEGAL-49: the switch ends the block, so leaving an arm is leaving the block, and no flag is needed.
    auto const wgsl = text_of(k_over_enum, target::wgsl);
    CHECK(wgsl.contains("    var case_result: f32;\n"));
    CHECK(!wgsl.contains("_left"));
    CHECK(wgsl.contains("    let w: f32 = case_result;\n"));
}

TEST("sgl emit - an arm with several patterns is one comma list in WGSL and one label per value elsewhere")
{
    constexpr auto source = "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                            "    let w = case 1:\n"
                            "        0 => 1.0\n"
                            "        1 or 2 => 0.5\n"
                            "        _ => 0.0\n"
                            "    return {color = float4(w, w, w, 1.0)}\n";
    CHECK(text_of(source, target::wgsl).contains("        case 1, 2: {\n"));

    auto const hlsl = text_of(source, target::hlsl_dx12);
    CHECK(hlsl.contains("    case 1:\n"));
    CHECK(hlsl.contains("    case 2:\n"));
}

TEST("sgl emit - an arm whose result is an assignment")
{
    // AST-127: `=` is looser than `=>`, so the arm arrives inside the assignment and its result is that statement.
    constexpr auto source = "enum k:\n"
                            "    a\n"
                            "    b\n"
                            "\n"
                            "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                            "    let mut total = 0.0\n"
                            "    case k.b:\n"
                            "        .a => total += 1.0\n"
                            "        _ => total = 0.5\n"
                            "    return {color = float4(total, total, total, 1.0)}\n";
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("        case k_a: {\n            total = total + 1.0;\n        }\n"));
    CHECK(wgsl.contains("        default: {\n            total = 0.5;\n        }\n"));
    // The case is a statement, so it needs no variable to carry a value out of the switch.
    CHECK(!wgsl.contains("case_result"));
}

TEST("sgl emit - an exit out of an arm crosses the switch and takes a flag")
{
    constexpr auto source = "enum k:\n"
                            "    a\n"
                            "    b\n"
                            "\n"
                            "fun guarded(m: k, base: float) -> float:\n"
                            "    let w = case m:\n"
                            "        .a => return 0.0\n"
                            "        _ => 1.0\n"
                            "    return base * w\n"
                            "\n"
                            "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                            "    let v = guarded(k.b, 2.0)\n"
                            "    return {color = float4(v, v, v, 1.0)}\n";
    // X5, with the switch as the construct it crosses: one bool, set in the arm and tested behind the switch.
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("guarded_left = true;"));
    CHECK(wgsl.contains("    if guarded_left {\n"));
}

TEST("sgl emit - a pattern that inlines a helper runs inside the branch that reaches it")
{
    // EVAL-68: a pattern is evaluated only where it is reached, so the helper's body stands behind the first arm's test.
    auto const wgsl = text_of("fun limit(x: float) -> float:\n"
                              "    if x > 1.0 => return 1.0\n"
                              "    return x\n"
                              "\n"
                              "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                              "    let v = case p.position.x:\n"
                              "        0.25 => 2.0\n"
                              "        limit(0.5) => 1.0\n"
                              "        _ => 0.0\n"
                              "    return {color = float4(v, v, v, 1.0)}\n",
                              target::wgsl);
    auto const first_test = wgsl.find("if case_value == 0.25 {");
    auto const helper = wgsl.find("var limit_result: f32;");
    CHECK(first_test >= 0);
    CHECK(helper > first_test);
    CHECK(wgsl.contains("if case_value == limit_result {"));
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::msl})
        CHECK(sgl::emit::dump_errors(emit_source(cc::string(k_edges)
                                                     + "fun limit(x: float) -> float:\n"
                                                       "    if x > 1.0 => return 1.0\n"
                                                       "    return x\n"
                                                       "\n"
                                                       "@pixel fun main_ps(p: pixel_input) -> frame:\n"
                                                       "    let v = case p.position.x:\n"
                                                       "        limit(0.5) => 1.0\n"
                                                       "        _ => 0.0\n"
                                                       "    return {color = float4(v, v, v, 1.0)}\n",
                                                 0, t))
              == "");
}
