#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::emit::target;

// CHK-538, LEGAL-72: what each statement of a local array indexed by data becomes in the text and in SPIR-V.

namespace
{
/// A texture encoder's per-texel loop: endpoints per subset, the subset read from a buffer, held as `how` says.
cc::string encoder(cc::string_view how)
{
    return cc::format("binding work:\n"
                      "    subsets: buffer[int]\n"
                      "    texels: buffer[float4]\n"
                      "    ends: mut buffer[float4]\n"
                      "\n"
                      "@compute(64) fun cs(@thread_id id: int3){{work}}:\n"
                      "    @indexed(.{}) let mut ep2 = float4[3].filled(float4(1.0))\n"
                      "    for k in 0 ..< 16:\n"
                      "        let s = work.subsets[16 * id.x + k]\n"
                      "        ep2[s] = min(ep2[s], work.texels[16 * id.x + k])\n"
                      "    for k in 0 ..< 3:\n"
                      "        work.ends[3 * id.x + k] = ep2[k]\n",
                      how);
}

constexpr u32 k_op_variable = 59;
constexpr u32 k_function = 7;

/// How many Function variables the SPIR-V that SGL writes itself declares for `source`'s `cs`.
isize function_variables_of(cc::string_view source)
{
    auto const e = sgl::compile_to_text(
        {.source = source, .entry_point = "cs", .target = target::hlsl_vulkan, .native = sgl::emit::native_route::require});
    REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
    auto const& words = e.value().spirv;
    auto count = isize(0);
    for (auto at = isize(5); at < words.size();)
    {
        auto const length = isize(words[at] >> 16);
        if ((words[at] & 0xffffu) == k_op_variable && length > 3 && words[at + 3] == k_function)
            ++count;
        at += length > 0 ? length : 1;
    }
    return count;
}
} // namespace

TEST("sgl emit - a .select array is a local per element, chosen among by the index, and a .memory one an array")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan, target::wgsl, target::msl})
    {
        auto const selected = emit_source(encoder("select"), 0, t);
        CHECK(sgl::emit::dump_errors(selected) == "");
        CHECK(!selected.text.contains("ep2[")).dump("text", selected.text);
        CHECK(selected.text.contains("ep2_2")).dump("text", selected.text);
        auto const kept = emit_source(encoder("memory"), 0, t);
        CHECK(sgl::emit::dump_errors(kept) == "");
        CHECK(kept.text.contains("ep2[s]")).dump("text", kept.text);
    }
    // a write is one store per element under its test, the last element taking every other index
    auto const hlsl = emit_source(encoder("select"), 0, target::hlsl_dx12).text;
    CHECK(hlsl.contains("if (chosen == 0)")).dump("text", hlsl);
    CHECK(hlsl.contains("else if (chosen == 1)")).dump("text", hlsl);
    CHECK(hlsl.contains("select(chosen == 0, ep2_0, select(chosen == 1, ep2_1, ep2_2))")).dump("text", hlsl);
}

TEST("sgl emit - SGL's own SPIR-V keeps a .memory array in a Function variable, and a .select one in none")
{
    CHECK(function_variables_of(encoder("memory")) == 1);
    CHECK(function_variables_of(encoder("select")) == 0);
}
