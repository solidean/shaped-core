#include "emit-test-support.hh"

using namespace sgl_test;
using sgl::emit::target;

// The 64-bit types as each target spells them (EMIT-190), what each target refuses (EMIT-109, EMIT-192), and where
// SGL's layout places them (EMIT-110, EMIT-111, EMIT-191).

namespace
{
/// Longs in a constant block, in a buffer's element and in arithmetic with literals past 32 bits.
constexpr cc::string_view k_longs = "require shader_int64\n"
                                    "\n"
                                    "struct cell:\n"
                                    "    weight: float\n"
                                    "    key: ulong\n"
                                    "    span: long2\n"
                                    "\n"
                                    "binding work:\n"
                                    "    seed: long\n"
                                    "    gain: float\n"
                                    "    pair: ulong2\n"
                                    "    @padding(.every_element) steps: long[2]\n"
                                    "    cells: mut buffer[cell]\n"
                                    "    keys: mut buffer[ulong3]\n"
                                    "\n"
                                    "@compute(64) fun run(@thread_id id: int3){work}:\n"
                                    "    let c = work.cells[id.x]\n"
                                    "    let n = id.x as uint as ulong\n"
                                    "    let k = (c.key << 3) ^ (c.key >> n) ^ 0x7fff'ffff'ffffu64\n"
                                    "    let s = -work.seed * 5'000'000'000 + c.span.x / c.span.y + work.steps[1]\n"
                                    "    let lowest = -9223372036854775807i64 - 1i64\n"
                                    "    work.cells[id.x] = {weight = c.weight, key = k + work.pair.y, span = "
                                    "long2(s << (n as long), min(s, lowest))}\n"
                                    "    work.keys[id.x] = ulong3(k, k << n, ~k)\n";

/// A double in a constant block, and a literal of one.
constexpr cc::string_view k_doubles = "require shader_f64\n"
                                      "\n"
                                      "binding work:\n"
                                      "    scale: double\n"
                                      "    values: mut buffer[double2]\n"
                                      "\n"
                                      "@compute(64) fun run(@thread_id id: int3){work}:\n"
                                      "    let v = work.values[id.x]\n"
                                      "    work.values[id.x] = v * work.scale + double2(0.1f64, -(v.x as float as "
                                      "double))\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "").dump("target", sgl::emit::to_string(t));
    return e.text;
}
} // namespace

TEST("sgl emit - a long is int64_t in HLSL and long in MSL, and its literals are suffixed as 64 bits")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const hlsl = text_of(k_longs, t);
        CHECK(hlsl.contains(" uint64_t key;\n"));
        CHECK(hlsl.contains("RWStructuredBuffer<cell> work_cells"));
        // EMIT-190: a literal past 32 bits keeps them, and the most negative long is no literal at all
        CHECK(hlsl.contains("-work.seed * 5000000000LL"));
        CHECK(hlsl.contains("^ 140737488355327ULL"));
        CHECK(hlsl.contains("-9223372036854775807LL - 1LL"));
        // EVAL-104: a count that is no literal is masked to its low six bits, which HLSL leaves undefined past 63
        CHECK(hlsl.contains("(c.key << 3ULL) ^ (c.key >> (n & 63))"));
        CHECK(hlsl.contains("~k"));
    }

    auto const msl = text_of(k_longs, target::msl);
    CHECK(msl.contains("    ulong key;\n"));
    CHECK(msl.contains("-work.seed * 5000000000L"));
    // MSL is C++, where a negative long shifted left is undefined, so it shifts the ulong of its bits
    CHECK(msl.contains("as_type<long>(as_type<ulong>(s) << (long(n) & 63))"));
    CHECK(msl.contains("static_assert(__builtin_offsetof(cell, key) == 8"));
}

TEST("sgl emit - a long and a long2 are 8 and 16 bytes, and a ulong3 in a buffer strides by 32")
{
    auto const vulkan = text_of(k_longs, target::hlsl_vulkan);
    CHECK(vulkan.contains("[[vk::offset(0)]] int64_t seed;\n"));
    CHECK(vulkan.contains("[[vk::offset(8)]] float gain;\n"));
    // EMIT-110: a ulong2 is a whole 16-byte row, so it starts on the next one
    CHECK(vulkan.contains("[[vk::offset(16)]] uint64_t2 pair;\n"));
    // EMIT-191: an array of longs in a constant block is a row per element, two longs to a row
    CHECK(vulkan.contains("[[vk::offset(32)]] int64_t2 steps[2];\n"));
    CHECK(vulkan.contains("[[vk::offset(8)]] uint64_t key;\n"));
    CHECK(vulkan.contains("[[vk::offset(16)]] int64_t2 span;\n"));
    CHECK(vulkan.contains("[[vk::offset(0)]] uint64_t3 value;\n"));
    CHECK(vulkan.contains("[[vk::offset(28)]] uint _pad1;\n"));
}

TEST("sgl emit - a 64-bit atomic is Interlocked* of an int64_t in HLSL, and Metal and WebGPU refuse it")
{
    constexpr auto source = cc::string_view("require shader_int64, shader_int64_atomics\n"
                                            "\n"
                                            "binding stats:\n"
                                            "    totals: mut buffer[atomic[ulong]]\n"
                                            "\n"
                                            "@compute(64) fun run(@thread_id id: int3){stats}:\n"
                                            "    let before = stats.totals[0].add(1u64 << 40)\n"
                                            "    stats.totals[1].store(stats.totals[2].load() + before)\n");
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const hlsl = text_of(source, t);
        CHECK(hlsl.contains("RWStructuredBuffer<uint64_t> stats_totals"));
        CHECK(hlsl.contains("InterlockedAdd(stats_totals[0], (1ULL << 40ULL), atomic_before);"));
        // a load is an `Or` of zero, which names its 64-bit zero so the 64-bit overload is the one taken
        CHECK(hlsl.contains("InterlockedOr(stats_totals[2], 0ULL, atomic_before_1);"));
    }
    CHECK(sgl::emit::dump_errors(emit_source(source, 0, target::msl))
              .contains("needs shader_int64_atomics, which Metal does not have"));
    CHECK(sgl::emit::dump_errors(emit_source(source, 0, target::wgsl))
              .contains("needs shader_int64_atomics, which WebGPU does not have"));
}

TEST("sgl emit - a double is double in HLSL, its literal suffixed L, and Metal and WebGPU refuse it")
{
    auto const hlsl = text_of(k_doubles, target::hlsl_dx12);
    CHECK(hlsl.contains("    double scale;\n"));
    CHECK(hlsl.contains("RWStructuredBuffer<double2> work_values"));
    // EMIT-190: a bare 0.1 is a float in HLSL, which would round it to 24 bits first
    CHECK(hlsl.contains("double2(0.1L, -double(float(v.x)))"));

    // EMIT-192 and EMIT-109
    CHECK(sgl::emit::dump_errors(emit_source(k_doubles, 0, target::msl))
              .contains("needs shader_f64, which Metal does "
                        "not have"));
    auto const wgsl = sgl::emit::dump_errors(emit_source(k_doubles, 0, target::wgsl));
    CHECK(wgsl.contains("needs shader_f64, which WebGPU does not have"));
    CHECK(sgl::emit::dump_errors(emit_source(k_longs, 0, target::wgsl))
              .contains("needs shader_int64, which WebGPU does "
                        "not have"));
}
