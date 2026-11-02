#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

namespace
{
/// A pixel stage that builds, fills, copies and indexes arrays, and keeps one in a struct.
constexpr cc::string_view k_arrays
    = "struct varyings:\n"
      "    @position position: hpos4\n"
      "    uv: float2\n"
      "\n"
      "struct ramp:\n"
      "    stops: float[2, 3]\n"
      "\n"
      "@pixel struct target:\n"
      "    color: float4\n"
      "\n"
      "@pixel fun ps(p: varyings) -> target:\n"
      "    let band = (p.uv.x * 3.0) as int\n"
      "    let weights = [0.25, 0.5, 0.25]\n"
      "    let mut r = ramp(stops = [weights, float[3].filled(1.0)])\n"
      "    r.stops[1, band] = weights[band]\n"
      "    return {color = float4(r.stops[0][band], r.stops[1, band], weights.length as float, 1.0)}\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

TEST("sgl emit - an array is WGSL's and MSL's array type, and HLSL's lengths after the name")
{
    auto const wgsl = text_of(k_arrays, target::wgsl);
    CHECK(wgsl.contains("    stops: array<array<f32, 3>, 2>,\n"));
    CHECK(wgsl.contains("var r: ramp = ramp(array<array<f32, 3>, 2>(weights, filled));\n"));
    CHECK(wgsl.contains("r.stops[1][band] = weights[band];\n"));
    // the length is a constant, and nothing of the array is read for it
    CHECK(wgsl.contains("f32(3)"));

    auto const hlsl = text_of(k_arrays, target::hlsl_dx12);
    CHECK(hlsl.contains("    float stops[2][3];\n"));
    // HLSL has no array expression, so a literal is a local filled element by element
    CHECK(hlsl.contains("    float weights[3];\n"
                        "    weights[0] = 0.25;\n"
                        "    weights[1] = 0.5;\n"
                        "    weights[2] = 0.25;\n"));
    CHECK(hlsl.contains("    float array_value[2][3];\n"
                        "    array_value[0] = weights;\n"
                        "    array_value[1] = filled;\n"
                        "    r.stops = array_value;\n"));
    CHECK(hlsl.contains("r.stops[1][band] = weights[band];\n"));

    auto const msl = text_of(k_arrays, target::msl);
    CHECK(msl.contains("    array<array<float, 3>, 2> stops;\n"));
}

TEST("sgl emit array - a const array is bound once per tree, however often and however deep it is read")
{
    // EMIT-165: a target would otherwise write the whole literal at every index
    constexpr auto source = "const table = [1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u]\n"
                            "\n"
                            "fun at(i: int) -> uint => table[i % 8]\n"
                            "\n"
                            "binding data:\n"
                            "    out_v: mut buffer[uint]\n"
                            "\n"
                            "@compute(1) fun main(@thread_id id: int3){data}:\n"
                            "    data.out_v[0] = at(id.x) + at(id.y) + table[id.z % 8]\n";
    for (auto const t : sgl::emit::all_targets())
    {
        auto const e = emit_source(source, 0, t);
        REQUIRE(sgl::emit::dump_errors(e) == "");
        // the literal's last element stands once, in the one binding every read takes
        auto const text = cc::string_view(e.text);
        auto count = 0;
        for (auto i = isize(0); i + 1 < text.size(); ++i)
            count += text[i] == '8' && text[i + 1] == 'u' ? 1 : 0;
        CHECK(count == 1).context(e.text);
    }
}
