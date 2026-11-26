#include "../check/check-test-support.hh"
#include "emit-test-support.hh"

#include <shaped-graphics-language/driver/compile_to_text.hh>

using namespace sgl_test;
using sgl::check::feature;
using sgl::emit::native_route;
using sgl::emit::target;

// `@early_depth_test` on a pixel stage (CHK-564 to CHK-567, EMIT-289): where it stands, what it refuses beside it, the
// feature it needs where the order of the tests changes a result, and what each target writes.
// What it does on a device is sg's pixel-semantics-test, on every route.

namespace
{
constexpr auto k_program = cc::string_view(R"(struct varyings:
    @position position: hpos4
    uv: float2

binding pass:
    visited: mut buffer[atomic[uint]]
    tint: float4

@early_depth_test
@pixel fun shade_ps(p: varyings){pass} -> float4 => pass.tint * p.uv.x

@early_depth_test
@pixel fun mark_ps(p: varyings){pass} -> float4:
    require early_depth_test
    pass.visited[0].add(1u)
    return pass.tint

@pixel fun plain_ps(p: varyings){pass} -> float4 => pass.tint
)");

[[nodiscard]] cc::result<sgl::emitted_source, cc::string> compiled(cc::string_view entry,
                                                                   target t,
                                                                   native_route native = native_route::text)
{
    return sgl::compile_to_text({.source = k_program, .entry_point = entry, .target = t, .native = native});
}

/// Whether a SPIR-V module states the execution mode EarlyFragmentTests.
[[nodiscard]] bool tests_early(cc::span<u32 const> words)
{
    for (auto at = isize(5); at < words.size(); at += isize(words[at] >> 16))
        if ((words[at] & 0xffffu) == 16 && words[at + 2] == 9) // OpExecutionMode, EarlyFragmentTests
            return true;
    return false;
}
} // namespace

TEST("sgl check - @early_depth_test stands on a pixel entry point alone, with no argument and no depth written")
{
    // CHK-564
    constexpr auto misplaced_source = R"(struct v:
    @position position: hpos4
binding pass:
    counts: mut buffer[atomic[uint]]
@early_depth_test
@compute(64) fun cs(@thread_id id: int3){pass}:
    pass.counts[0].add(1u)
@early_depth_test(true)
@pixel fun argued_ps(p: v) -> float4 => float4(1.0)
)";
    auto const misplaced = reports_for(misplaced_source);
    CHECK(misplaced.contains("invalid-attribute-arguments user:[early_depth_test] @early_depth_test takes no argument, "
                             "and stands on a @pixel entry point alone"))
        .dump("report", misplaced);
    // CHK-565: the tests and their writes are decided before the stage runs
    for (auto const member : {"@depth z: float", "@depth(.greater_equal) z: float", "@sample_mask z: uint"})
    {
        auto const source = cc::format(R"(struct v:
    @position position: hpos4
@pixel struct deep:
    color: float4
    {}
@early_depth_test
@pixel fun deep_ps(p: v) -> deep:
    return undefined()
)",
                                       member);
        auto const report = reports_for(source);
        CHECK(report.contains("invalid-entry-point user:[early_depth_test] @early_depth_test decides the depth and "
                              "stencil tests before the stage runs"))
            .dump("report", report)
            .dump("member", member);
    }
}

TEST("sgl emit - @early_depth_test is each target's own attribute, and WGSL writes it only where it changes nothing")
{
    for (auto const t : {target::hlsl_dx12, target::hlsl_vulkan})
    {
        auto const e = compiled("shade_ps", t);
        REQUIRE(e.has_value()).dump("error", e.has_error() ? e.error() : cc::string());
        CHECK(e.value().text.contains("[earlydepthstencil]\nshade_ps_target shade_ps(")).dump("text", e.value().text);
        CHECK(!e.value().features.has(feature::early_depth_test));
    }
    auto const msl = compiled("shade_ps", target::msl);
    REQUIRE(msl.has_value()).dump("error", msl.has_error() ? msl.error() : cc::string());
    CHECK(msl.value().text.contains("[[early_fragment_tests]] fragment shade_ps_target shade_ps("))
        .dump("text", msl.value().text);

    // CHK-567: a stage that writes nothing and never discards is tested early or late with one result
    auto const wgsl = compiled("shade_ps", target::wgsl);
    REQUIRE(wgsl.has_value()).dump("error", wgsl.has_error() ? wgsl.error() : cc::string());
    CHECK(!wgsl.value().text.contains("early")).dump("text", wgsl.value().text);

    // one that writes memory needs the feature, which WebGPU has on no device (EMIT-109)
    auto const marked = compiled("mark_ps", target::hlsl_dx12);
    REQUIRE(marked.has_value()).dump("error", marked.has_error() ? marked.error() : cc::string());
    CHECK(marked.value().features.has(feature::early_depth_test));
    auto const refused = compiled("mark_ps", target::wgsl);
    REQUIRE(refused.has_error());
    CHECK(refused.error().contains("mark_ps needs early_depth_test, which WebGPU does not have"))
        .dump("error", refused.error());

    // a stage without it states nothing
    auto const plain = compiled("plain_ps", target::hlsl_dx12);
    REQUIRE(plain.has_value()).dump("error", plain.has_error() ? plain.error() : cc::string());
    CHECK(!plain.value().text.contains("earlydepthstencil")).dump("text", plain.value().text);
}

TEST("sgl emit - SGL's own SPIR-V forces the early tests by the execution mode, and its DXIL by the shader flag")
{
    // EMIT-289: as DXC compiles `[earlydepthstencil]`
    auto const marked = compiled("mark_ps", target::hlsl_vulkan, native_route::require);
    REQUIRE(marked.has_value()).dump("error", marked.has_error() ? marked.error() : cc::string());
    CHECK(tests_early(marked.value().spirv));
    auto const plain = compiled("plain_ps", target::hlsl_vulkan, native_route::require);
    REQUIRE(plain.has_value()).dump("error", plain.has_error() ? plain.error() : cc::string());
    CHECK(!tests_early(plain.value().spirv));

    auto const dx12 = compiled("mark_ps", target::hlsl_dx12, native_route::require);
    REQUIRE(dx12.has_value()).dump("error", dx12.has_error() ? dx12.error() : cc::string());
    CHECK(!dx12.value().dxil.empty());
}
