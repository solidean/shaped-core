#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::emit::target;

// EMIT-141 and EMIT-146 to EMIT-149: a subgroup operation is the target's own, the subgroup's stage inputs are what
// each target hands over, the preferred size is dx12's alone, and the uniform load is WGSL's own or a barrier pair.

namespace
{
/// A reduction over the quad and the subgroup, each stage input, and the shape a single-pass reduction ends in.
constexpr cc::string_view k_reduce
    = "require subgroups\n"
      "\n"
      "binding work:\n"
      "    input: buffer[float]\n"
      "    output: mut buffer[float]\n"
      "    counter: mut buffer[atomic[uint]]\n"
      "\n"
      "@workgroup binding spd:\n"
      "    finished: uint\n"
      "\n"
      "@compute(64) @preferred_subgroup_size(32) fun reduce(@local_thread_index li: int, "
      "@subgroup_size size: int, @subgroup_invocation_id lane: int){work, spd}:\n"
      "    let v = work.input[li]\n"
      "    let swapped = quad_swap_x(v)\n"
      "    let total = subgroup_add(swapped)\n"
      "    if li == 0:\n"
      "        spd.finished = work.counter[0].add(1u)\n"
      "    if workgroup_uniform_load(spd.finished) == 7u:\n"
      "        workgroup_barrier()\n"
      "    work.output[li] = total + (size as float) + (lane as float)\n";

cc::string text_of(cc::string_view source, target t)
{
    auto const e = emit_source(source, 0, t);
    CHECK(sgl::emit::dump_errors(e) == "");
    return e.text;
}
} // namespace

TEST("sgl emit - a reduction and a quad swap are each target's own")
{
    auto const dx12 = text_of(k_reduce, target::hlsl_dx12);
    CHECK(dx12.contains("    const float swapped = QuadReadAcrossX(v);\n"));
    CHECK(dx12.contains("    const float total = WaveActiveSum(swapped);\n"));
    // DXC writes the same intrinsics as SPIR-V's group operations for vulkan
    auto const vulkan = text_of(k_reduce, target::hlsl_vulkan);
    CHECK(vulkan.contains("    const float swapped = QuadReadAcrossX(v);\n"));
    CHECK(vulkan.contains("    const float total = WaveActiveSum(swapped);\n"));

    auto const wgsl = text_of(k_reduce, target::wgsl);
    CHECK(wgsl.starts_with("// SGL compute entry point 'reduce', written as WGSL.\n"
                           "// Generated: the SGL source is what to edit.\n\n"
                           "enable subgroups;\n\n"));
    CHECK(wgsl.contains("    let swapped: f32 = quadSwapX(v);\n"));
    CHECK(wgsl.contains("    let total: f32 = subgroupAdd(swapped);\n"));

    // MSL has one quad shuffle, which a swap takes by the lane bits that differ
    auto const msl = text_of(k_reduce, target::msl);
    CHECK(msl.contains("    const float swapped = quad_shuffle_xor(v, 1);\n"));
    CHECK(msl.contains("    const float total = simd_sum(swapped);\n"));
}

TEST("sgl emit - the subgroup's stage inputs are read where HLSL has no semantic for them")
{
    auto const dx12 = text_of(k_reduce, target::hlsl_dx12);
    CHECK(dx12.contains("void reduce(uint li_in : SV_GroupIndex)\n"));
    CHECK(dx12.contains("    const int size_1 = int(WaveGetLaneCount());\n"));
    CHECK(dx12.contains("    const int lane = int(WaveGetLaneIndex());\n"));

    auto const wgsl = text_of(k_reduce, target::wgsl);
    CHECK(wgsl.contains("@builtin(subgroup_size) size_1_in: u32, @builtin(subgroup_invocation_id) lane_in: u32"));
    CHECK(wgsl.contains("    let lane: i32 = i32(lane_in);\n"));

    auto const msl = text_of(k_reduce, target::msl);
    CHECK(msl.contains("uint size_1_in [[threads_per_simdgroup]], uint lane_in [[thread_index_in_simdgroup]]"));
    CHECK(msl.contains("    const int lane = int(lane_in);\n"));
}

TEST("sgl emit - a preferred subgroup size is the range form on dx12, and reaches the host beside the text")
{
    CHECK(text_of(k_reduce, target::hlsl_dx12).contains("[numthreads(64, 1, 1)]\n[WaveSize(4, 128, 32)]\nvoid reduce("));
    // no other text states it: sg's vulkan asks the pipeline instead, and WGSL and MSL have no way to ask
    CHECK(!text_of(k_reduce, target::hlsl_vulkan).contains("WaveSize"));

    for (auto const t : sgl::emit::all_targets())
    {
        auto const emitted = sgl::compile_to_text({.source = k_reduce, .entry_point = "reduce", .target = t});
        REQUIRE(emitted.has_value());
        CHECK(emitted.value().preferred_subgroup_size == 32);
    }
}

TEST("sgl emit - the uniform load is WGSL's own, and a barrier on either side of a read elsewhere")
{
    auto const wgsl = text_of(k_reduce, target::wgsl);
    CHECK(wgsl.contains("    if workgroupUniformLoad(&spd_finished) == 7u {\n"
                        "        workgroupBarrier();\n"
                        "    }\n"));

    // no thread stores to the memory before every one has read it, so the value is the same in each
    auto const dx12 = text_of(k_reduce, target::hlsl_dx12);
    CHECK(dx12.contains("    GroupMemoryBarrierWithGroupSync();\n"
                        "    uint uniform_load = spd_finished;\n"
                        "    GroupMemoryBarrierWithGroupSync();\n"
                        "    if (uniform_load == 7u)\n"));

    auto const msl = text_of(k_reduce, target::msl);
    CHECK(msl.contains("    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                       "    uint uniform_load = spd_finished;\n"
                       "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
                       "    if (uniform_load == 7u)\n"));
}

TEST("sgl emit - the rest of the subgroup family, with a lane converted to the target's lane type")
{
    constexpr auto family = "require subgroups\n"
                            "\n"
                            "binding work:\n"
                            "    output: mut buffer[uint]\n"
                            "\n"
                            "@compute(64) fun cs(@local_thread_index li: int){work}:\n"
                            "    let x = li as uint\n"
                            "    let b = subgroup_ballot(x > 3u)\n"
                            "    let s = subgroup_inclusive_add(x)\n"
                            "    let f = subgroup_broadcast(x, 5)\n"
                            "    let d = subgroup_shuffle_down(x, 2)\n"
                            "    let q = quad_broadcast(x, 3)\n"
                            "    let o = subgroup_bit_or(x)\n"
                            "    work.output[li] = b.x + s + f + d + q + o\n";
    auto const dx12 = text_of(family, target::hlsl_dx12);
    CHECK(dx12.contains("uint sgl_subgroup_inclusive_add(uint x)\n{\n    return WavePrefixSum(x) + x;\n}\n"));
    CHECK(dx12.contains("    const uint4 b = WaveActiveBallot(x > 3u);\n"));
    CHECK(dx12.contains("    const uint f = WaveReadLaneAt(x, uint(5));\n"));
    CHECK(dx12.contains("    const uint d = WaveReadLaneAt(x, WaveGetLaneIndex() + uint(2));\n"));
    CHECK(dx12.contains("    const uint q = QuadReadLaneAt(x, uint(3));\n"));
    CHECK(dx12.contains("    const uint o = WaveActiveBitOr(x);\n"));

    auto const wgsl = text_of(family, target::wgsl);
    CHECK(wgsl.contains("    let s: u32 = subgroupInclusiveAdd(x);\n"));
    CHECK(wgsl.contains("    let f: u32 = subgroupBroadcast(x, u32(5));\n"));
    CHECK(wgsl.contains("    let d: u32 = subgroupShuffleDown(x, u32(2));\n"));
    CHECK(wgsl.contains("    let q: u32 = quadBroadcast(x, u32(3));\n"));

    auto const msl = text_of(family, target::msl);
    CHECK(msl.contains("    ulong v = ulong(simd_ballot(b));\n"));
    CHECK(msl.contains("    const uint4 b = sgl_subgroup_ballot(x > 3u);\n"));
    CHECK(msl.contains("    const uint f = simd_broadcast(x, ushort(5));\n"));
    CHECK(msl.contains("    const uint d = simd_shuffle_down(x, ushort(2));\n"));
    CHECK(msl.contains("    const uint o = simd_or(x);\n"));
}
