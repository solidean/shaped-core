#include "check-test-support.hh"

#include <clean-core/string/format.hh>

using namespace sgl_test;

// CHK-187 and CHK-188: `@stages` restricts where a function may be reached from, judged per entry point after inlining.

namespace
{
constexpr cc::string_view k_resources = "binding tex:\n"
                                        "    src: texture2d[float4]\n"
                                        "    smp: sampler\n"
                                        "    dst: out image2d[.rgba8_unorm]\n"
                                        "\n"
                                        "struct pixel_input:\n"
                                        "    @position position: hpos4\n"
                                        "\n"
                                        "@pixel struct target:\n"
                                        "    color: float4\n"
                                        "\n";

cc::string with_entry(cc::string_view functions, cc::string_view stage_line, cc::string_view body)
{
    return cc::format("{}{}{}\n{}", k_resources, functions, stage_line, body);
}

cc::string compute(cc::string_view functions, cc::string_view body)
{
    return with_entry(functions, "@compute(8, 8) fun cs(@thread_id id: int3){tex}:", body);
}

cc::string pixel(cc::string_view functions, cc::string_view body)
{
    return with_entry(functions, "@pixel fun ps(p: pixel_input){tex} -> target:", body);
}
} // namespace

TEST("sgl check - a pixel-only builtin is refused in any other stage, after inlining")
{
    constexpr auto sample = "    let c = DEBUG_sample(tex.src, float2(0.5, 0.5), tex.smp)\n";
    CHECK(reports_for(pixel("", cc::format("{}    return {{color = c}}\n", sample))) == "");
    CHECK(reports_for(compute("", cc::format("{}    DEBUG_store(tex.dst, int2(0, 0), c)\n", sample)))
              .contains("stage-not-allowed"));
    CHECK(reports_for(compute("", cc::format("{}    DEBUG_store(tex.dst, int2(0, 0), c)\n", sample)))
              .contains("cs is a compute entry point, and DEBUG_sample is @stages without it"));

    // Reached through a function that says nothing about stages, it is still the entry point that decides.
    constexpr auto shade = "fun shade(){tex} -> float4 => DEBUG_sample(tex.src, float2(0.5, 0.5), tex.smp)\n\n";
    CHECK(reports_for(pixel(shade, "    return {color = shade()}\n")) == "");
    CHECK(reports_for(compute(shade, "    DEBUG_store(tex.dst, int2(0, 0), shade())\n")).contains("stage-not-allowed"));
}

TEST("sgl check - @stages restricts a program's own function the same way")
{
    constexpr auto lit = "@stages(.pixel) fun lit(c: float4) -> float4 => c * 0.5\n\n";
    CHECK(reports_for(pixel(lit, "    return {color = lit(float4(1.0, 1.0, 1.0, 1.0))}\n")) == "");
    CHECK(reports_for(compute(lit, "    DEBUG_store(tex.dst, int2(0, 0), lit(float4(1.0, 1.0, 1.0, 1.0)))\n"))
              .contains("lit is @stages without it"));

    constexpr auto either = "@stages(.vertex, .compute) fun half(c: float4) -> float4 => c * 0.5\n\n";
    CHECK(reports_for(compute(either, "    DEBUG_store(tex.dst, int2(0, 0), half(float4(1.0, 1.0, 1.0, 1.0)))\n")) == "");

    CHECK(reports_for(pixel("@stages(pixel) fun f() -> float => 1.0\n\n", "    return {color = float4(1.0, 1.0, 1.0, "
                                                                          "1.0)}\n"))
              .contains("invalid-attribute-arguments"));
}
