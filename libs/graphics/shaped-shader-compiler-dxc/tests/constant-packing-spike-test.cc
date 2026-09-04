#include <clean-core/common/log.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-shader-compiler-dxc/all.hh>

using namespace cc::primitive_defines;

// Q14: how does a constant buffer pack, one rule at a time?
//
// The binding preprocessor generates a C++ mirror of an annotated constant block, and the mirror has to
// reproduce HLSL's layout rather than C++'s — see libs/graphics/shaped-shader-library/docs/binding-preprocessor.md.
// Every rule below is a silent wrong number if guessed, so none of them is guessed here.
//
// The instrument is `block_size`, which reflection already reports for a uniform_buffer binding, and each case
// is a struct whose total differs between the candidate layouts.
// One rule per case, so a failure names the rule rather than "the packing changed".
//
// Both targets are checked, because the SPIR-V arm compiles with -fvk-use-dx-layout precisely so that one
// CPU-side struct can serve both — and that flag is only worth having if it actually holds.
//
// Three of these came out other than the folklore says, which is the whole reason for measuring:
//   - an array's stride is 16, but its LAST element does not claim the rest of its row;
//   - a nested struct is NOT row-aligned and NOT rounded up to a row;
//   - `row_major float3x3` followed by anything is not portable at all — SPIR-V rejects the module.

#ifdef CC_OS_WINDOWS

namespace
{
/// Wraps `members` in a constant buffer the entry point reads, so reflection reports the block.
/// `prelude` is whatever the members need declared first, which is only ever a nested struct type.
[[nodiscard]] cc::string constant_block(cc::string_view members, cc::string_view prelude)
{
    return cc::format(R"(
{}
struct block
{{
{}
}};

ConstantBuffer<block> Params : register(b0, space0);
RWStructuredBuffer<float> Out : register(u0, space0);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{{
    Out[0] = Params.probe;
}}
)",
                      prelude, members);
}

/// The `block_size` DXC reports for a constant block holding `members`, or -1 when it will not compile.
[[nodiscard]] isize block_size_of(cc::string_view members, ssc::dxc::compile_target target, cc::string_view prelude = "")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto const source = constant_block(members, prelude);
    auto result = comp.value().compile(
        {.source = source, .entry_point = cc::string("main"), .stage = sg::shader_stage::compute}, {.target = target});

    if (result.has_error())
    {
        CC_LOG_INFO("[spike] Q14 refused: {}", result.error().to_string());
        return -1;
    }

    for (auto const& b : result.value().bindings)
        if (b.type == sg::binding_type::uniform_buffer && b.block_size.has_value())
            return b.block_size.value();

    return -1;
}

/// One rule, checked on both targets, which is where a portability difference would show up.
/// `members` ends with a `float probe;` so the entry point has something to read, and every expected size
/// accounts for it.
void check_rule(cc::string_view what, cc::string_view members, isize expected, cc::string_view prelude = "")
{
    for (auto const target : {ssc::dxc::compile_target::dxil, ssc::dxc::compile_target::spirv})
    {
        auto const size = block_size_of(members, target, prelude);
        auto const name = target == ssc::dxc::compile_target::dxil ? "dxil" : "spirv";
        CC_LOG_INFO("[spike] Q14 {} {}: block_size={} (expected {})", name, what, size, expected);
        CHECK(size == expected);
    }
}
} // namespace

TEST("portable-hlsl spike - Q14 an element may not straddle a 16-byte row")
{
    // float2 at 0..8, float3 would cross the row so it starts at 16, probe at 28 -> 32.
    // The naive C++ transcription would be 8 + 12 + 4 = 24.
    check_rule("no straddling", "    float2 a;\n    float3 b;\n    float probe;", 32);

    // float at 0, float4 cannot straddle so it starts at 16 and ends at 32, probe at 32 -> 48.
    check_rule("a float4 is row-aligned", "    float a;\n    float4 b;\n    float probe;", 48);

    // And what does NOT move: a float3 after a float fits in the same row, so nothing is padded.
    check_rule("a row is filled before it is left", "    float a;\n    float3 b;\n    float probe;", 32);
}

TEST("portable-hlsl spike - Q14b an array's stride is 16, but its last element does not claim its row")
{
    // The half everyone knows: a[0] at 0 and a[1] at 16 rather than at 4.
    // The half that surprises: `probe` lands at 20, right after a[1], rather than being pushed to 32.
    // So an array of N takes (N-1)*16 + sizeof(element), and the next member packs against that.
    check_rule("two floats", "    float a[2];\n    float probe;", 32);
    check_rule("three floats", "    float a[3];\n    float probe;", 48);

    // Which the total confirms from the other side: one more element adds exactly one row.
    check_rule("one float", "    float a[1];\n    float probe;", 16);
}

TEST("portable-hlsl spike - Q14c a matrix is rows, and float3x3 is not portable at all")
{
    // float4x4 needs no padding, since a float4 fills its row exactly: 64 bytes, probe at 64 -> 80.
    check_rule("float4x4 fills its rows", "    row_major float4x4 m;\n    float probe;", 80);

    // float3x3 is three rows of float3.
    // DXIL packs `probe` into the last row's tail, at 44, giving 48.
    // SPIR-V REFUSES the module: under -fvk-use-dx-layout the matrix is laid out as ending at 44 while its
    // extent is 48, and the validator calls that an overlap.
    //
    // So a float3x3 in a constant block is not a layout to reproduce — it is a construct a portable shader
    // cannot contain, and the pass rejects it rather than generating a mirror for something that will not build.
    CHECK(block_size_of("    row_major float3x3 m;\n    float probe;", ssc::dxc::compile_target::dxil) == 48);
    CHECK(block_size_of("    row_major float3x3 m;\n    float probe;", ssc::dxc::compile_target::spirv) == -1);
}

TEST("portable-hlsl spike - Q14d a nested struct is neither row-aligned nor rounded up")
{
    // The folklore says a nested struct starts a row and occupies whole rows.
    // It does not, on either target: the inner float sits at 0 and `probe` follows at 4, for 8 -> 16.
    check_rule("a nested struct packs inline", "    struct { float x; } s;\n    float probe;", 16);

    // And a named struct type behaves the same, so it is not an artefact of declaring one inline.
    check_rule("a named nested struct too", "    inner s;\n    float probe;", 16, "struct inner { float x; };");
}

TEST("portable-hlsl spike - Q14e a bool is four bytes, not one")
{
    // Which is the reason sr::gpu_boolean exists.
    check_rule("bool is four bytes", "    bool a;\n    float probe;", 16);
}

#endif
