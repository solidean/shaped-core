#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/describe.hh>

using namespace sgl_test;
using sgl::emit::target;

// The 16-bit types as each target spells them (EMIT-140, EMIT-141), and where SGL's layout places them (EMIT-110,
// EMIT-111).

namespace
{
/// Halves in a constant block, in a buffer's element and in arithmetic with a literal.
constexpr cc::string_view k_halves = "require shader_f16\n"
                                     "\n"
                                     "struct sample:\n"
                                     "    weight: float\n"
                                     "    tint: half3\n"
                                     "    scale: half\n"
                                     "\n"
                                     "binding work:\n"
                                     "    gain: half\n"
                                     "    offset: float\n"
                                     "    bias: half3\n"
                                     "    input: buffer[sample]\n"
                                     "    halves: mut buffer[half2]\n"
                                     "\n"
                                     "@compute(64) fun run(@thread_id id: int3){work}:\n"
                                     "    let s = work.input[id.x]\n"
                                     "    let v = s.tint * 0.5 + work.bias * work.gain\n"
                                     "    work.halves[id.x] = half2(v.x + (s.weight as half), -0.1f16)\n";

/// 16-bit integers, whose scalar arithmetic MSL computes as an int.
constexpr cc::string_view k_shorts = "require shader_int16\n"
                                     "\n"
                                     "binding work:\n"
                                     "    counts: mut buffer[ushort2]\n"
                                     "\n"
                                     "@compute(64) fun run(@thread_id id: int3){work}:\n"
                                     "    let c = work.counts[id.x]\n"
                                     "    let n = c.x * c.y + 3u16\n"
                                     "    let s = -(n as short)\n"
                                     "    work.counts[id.x] = ushort2(n, sign(s) as ushort)\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "").dump("target", sgl::emit::to_string(t));
    return e.text;
}

sgl::module_description described(cc::string_view source)
{
    auto const d = sgl::describe({.source = source});
    if (d.has_error())
        FAIL(d.error());
    return d.value();
}
} // namespace

TEST("sgl emit - a half is float16_t in HLSL, f16 in WGSL and half in MSL, and a literal of one is a construction")
{
    auto const hlsl = text_of(k_halves, target::hlsl_dx12);
    CHECK(hlsl.contains("    float16_t3 tint;\n"));
    CHECK(hlsl.contains("RWStructuredBuffer<float16_t2> work_halves"));
    CHECK(hlsl.contains("const float16_t3 v = s.tint * float16_t(0.5) + work.bias * work.gain;\n"));
    CHECK(hlsl.contains("float16_t2(v.x + float16_t(s.weight), float16_t(-0.0999755859375))"));
    // EMIT-140: never `half`, which means a 32-bit float to a DXC without -enable-16bit-types
    CHECK(!hlsl.contains("half"));

    auto const wgsl = text_of(k_halves, target::wgsl);
    // EMIT-141: ahead of every declaration
    CHECK(wgsl.starts_with("// SGL"));
    CHECK(wgsl.find("enable f16;\n") < wgsl.find("struct"));
    CHECK(wgsl.contains("let v: vec3h = s.tint * f16(0.5) + work.bias * work.gain;\n"));
    CHECK(wgsl.contains("vec2h(v.x + f16(s.weight), f16(-0.0999755859375))"));

    auto const msl = text_of(k_halves, target::msl);
    CHECK(msl.contains("const half3 v = s.tint * half(0.5) + work.bias_ * work.gain;\n"));
    CHECK(msl.contains("half2(v.x + half(s.weight), half(-0.0999755859375))"));

    // an entry point that holds no half enables nothing
    CHECK(!text_of("binding w:\n    v: mut buffer[float]\n\n@compute(64) fun run(@thread_id id: int3){w}:\n"
                   "    w.v[id.x] = 1.0\n",
                   target::wgsl)
               .contains("enable f16"));
}

TEST("sgl emit - a short is int16_t in HLSL and short in MSL, whose scalar arithmetic converts back from an int")
{
    auto const hlsl = text_of(k_shorts, target::hlsl_dx12);
    CHECK(hlsl.contains("RWStructuredBuffer<uint16_t2> work_counts"));
    CHECK(hlsl.contains("const uint16_t n = c.x * c.y + uint16_t(3u);\n"));
    CHECK(hlsl.contains("const int16_t s = -int16_t(n);\n"));
    // HLSL's sign gives an int
    CHECK(hlsl.contains("uint16_t(int16_t(sign(s)))"));

    auto const msl = text_of(k_shorts, target::msl);
    // C++ promotes a ushort to an int, whose product of two of them could overflow, so it multiplies as a uint
    CHECK(msl.contains("const ushort n = ushort(ushort(uint(c.x) * c.y) + ushort(3u));\n"));
    CHECK(msl.contains("const short s = short(-short(n));\n"));
    CHECK(msl.contains("clamp(s, short(-1), short(1))"));

    // EMIT-109: WGSL has no 16-bit integer
    auto const wgsl = emit_source(k_shorts, 0, target::wgsl);
    CHECK(sgl::emit::dump_errors(wgsl).contains("target-lacks-feature"));
    CHECK(sgl::emit::dump_errors(wgsl).contains("shader_int16"));
}

TEST("sgl emit layout - a 16-bit value packs at 2 bytes in a block and in a buffer's element")
{
    // EMIT-110: `gain` at 0, `offset` at 4, and `bias` at 8, 6 bytes inside the first row
    auto const vulkan = text_of(k_halves, target::hlsl_vulkan);
    CHECK(vulkan.contains("    [[vk::offset(0)]] float16_t gain;\n"
                          "    [[vk::offset(4)]] float offset;\n"
                          "    [[vk::offset(8)]] float16_t3 bias;\n"));
    // EMIT-111: `tint` at 4 and `scale` at 10, 12 bytes in all
    CHECK(vulkan.contains("    [[vk::offset(0)]] float weight;\n"
                          "    [[vk::offset(4)]] float16_t3 tint;\n"
                          "    [[vk::offset(10)]] float16_t scale;\n"));

    // WGSL aligns a vec3h at 8, so the element is split into its halves; the block it places natively
    auto const wgsl = text_of(k_halves, target::wgsl);
    CHECK(wgsl.contains("struct sample_memory {\n"
                        "    weight: f32,\n"
                        "    tint_x: f16,\n"
                        "    tint_y: f16,\n"
                        "    tint_z: f16,\n"
                        "    scale: f16,\n"
                        "}\n"));
    CHECK(wgsl.contains("struct work_data {\n    gain: f16,\n    offset: f32,\n    bias: vec3h,\n}\n"));

    // MSL packs the half3, which its own rule aligns at 8 too
    auto const msl = text_of(k_halves, target::msl);
    CHECK(msl.contains("struct sample_memory\n{\n    float weight;\n    packed_half3 tint;\n    half scale;\n};\n"));

    auto const d = described(k_halves);
    REQUIRE(d.memory_structs.size() == 1);
    CHECK(d.memory_structs[0].size == 12);
    CHECK(d.memory_structs[0].members[1].offset == 4);
    CHECK(d.memory_structs[0].members[2].offset == 10);
    CHECK(d.bindings[0].members[3].stride == 12);
    CHECK(d.bindings[0].members[4].stride == 4);
}

TEST("sgl emit layout - a 2-byte gap a half leaves is padded with a 16-bit field")
{
    // EMIT-110: `tint` at 4, which WGSL and MSL align at 8, so both write the block's memory form
    constexpr auto source = cc::string_view("require shader_f16\n"
                                            "\n"
                                            "binding work:\n"
                                            "    gain: half\n"
                                            "    tint: float2\n"
                                            "    results: mut buffer[float2]\n"
                                            "\n"
                                            "@compute(64) fun run(@thread_id id: int3){work}:\n"
                                            "    work.results[id.x] = work.tint * (work.gain as float)\n");
    auto const vulkan = text_of(source, target::hlsl_vulkan);
    CHECK(vulkan.contains("    [[vk::offset(0)]] float16_t gain;\n    [[vk::offset(4)]] float2 tint;\n"));
    auto const wgsl = text_of(source, target::wgsl);
    CHECK(wgsl.contains("    gain: f16,\n    _pad0: f16,\n    tint_x: f32,\n    tint_y: f32,\n"));
    auto const msl = text_of(source, target::msl);
    CHECK(msl.contains("    half gain;\n    ushort _pad0;\n    packed_float2 tint;\n"));
}
