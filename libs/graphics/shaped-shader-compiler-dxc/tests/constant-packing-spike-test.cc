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
// Two of these came out other than the folklore says, which is the whole reason for measuring:
//   - an array's stride is 16, but its LAST element does not claim the rest of its row;
//   - `row_major float3x3` followed by anything is not portable at all — SPIR-V rejects the module.
//
// The SPIR-V half needs nothing from the Windows SDK, so it runs everywhere and only the DXIL legs are guarded:
// `impl::reflect_spirv` is what a SPIR-V target selects, and reading `block_size` off it is the only observable
// check of `-fvk-use-dx-layout` anywhere in the repo.

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

/// One rule, checked on every target available here, which is where a portability difference would show up.
/// `members` ends with a `float probe;` so the entry point has something to read, and every expected size
/// accounts for it.
///
/// DXIL reflection reads the container beside the bytecode through the Windows SDK's d3d12shader.h, which the
/// Linux DXC release does not ship — so that arm is Windows-only and the SPIR-V one is not.
void check_rule(cc::string_view what, cc::string_view members, isize expected, cc::string_view prelude = "")
{
    auto const spirv = block_size_of(members, ssc::dxc::compile_target::spirv, prelude);
    CC_LOG_INFO("[spike] Q14 spirv {}: block_size={} (expected {})", what, spirv, expected);
    CHECK(spirv == expected);

#ifdef CC_OS_WINDOWS
    auto const dxil = block_size_of(members, ssc::dxc::compile_target::dxil, prelude);
    CC_LOG_INFO("[spike] Q14 dxil {}: block_size={} (expected {})", what, dxil, expected);
    CHECK(dxil == expected);
#endif
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
    // The rejection is the SPIR-V validator's, so it is the half worth having everywhere.
    CHECK(block_size_of("    row_major float3x3 m;\n    float probe;", ssc::dxc::compile_target::spirv) == -1);

#ifdef CC_OS_WINDOWS
    CHECK(block_size_of("    row_major float3x3 m;\n    float probe;", ssc::dxc::compile_target::dxil) == 48);
#endif
}

TEST("portable-hlsl spike - Q14d a nested struct starts a row, but does not round up to one")
{
    // Two halves, and each needs a case where the two readings differ across a row boundary — a case that
    // lands on the same total either way proves nothing, which is how the second half was got wrong first.
    //
    // The START is row-aligned: s cannot pack into x's row, so v lands at 16 and probe at 20, for 24 -> 32.
    // Packed inline, v would land at 4 and probe at 8, for 12 -> 16.
    check_rule("a nested struct starts a row", "    float x;\n    struct { float v; } s;\n    float probe;", 32);

    // The END is not: probe packs against the struct's last member rather than being pushed to the next row.
    // s at 0 either way, so this is only about what follows it: probe at 4 gives 8 -> 16, probe at 16 gives 32.
    check_rule("a nested struct does not round up", "    struct { float x; } s;\n    float probe;", 16);

    // And a named struct type behaves the same, so neither half is an artefact of declaring one inline.
    check_rule("a named nested struct too", "    inner s;\n    float probe;", 16, "struct inner { float x; };");
}

TEST("portable-hlsl spike - Q14f an array starts a row, and the total rounds up to one")
{
    // An array does start its own row: x at 0, a[0] at 16 rather than at 4, a[1] at 32, probe at 36 -> 48.
    // If the array had packed against x, the total would be 32.
    check_rule("an array starts a row", "    float x;\n    float a[2];\n    float probe;", 48);

    // And the block's own total rounds up to a whole row, which is why every case above is a multiple of 16.
    check_rule("the total rounds up to a row", "    float probe;", 16);
}

TEST("portable-hlsl spike - Q14e a bool is four bytes, not one")
{
    // Which is the reason sr::gpu_boolean exists.
    check_rule("bool is four bytes", "    bool a;\n    float probe;", 16);
}

TEST("portable-hlsl spike - Q14g a matrix is measured by its orientation, and by what its last row leaves")
{
    // A matrix stores V vectors of M components -- row-major stores R vectors of C, column-major stores C of R
    // -- laid out at a 16-byte stride, so its extent is (V - 1) * 16 + M * 4.
    //
    // The orientation is therefore part of the layout rather than a detail of it, and it comes from a compile
    // flag (`#pragma pack_matrix`, `-Zpr`) unless the declaration states one.
    // That is why the pass keys its table on the qualifier and refuses a bare matrix: the same source would
    // otherwise mirror to two different structs depending on how it was compiled.

    // Row-major, so the COLUMN count is what fills a stored vector.
    check_rule("row_major float1x4", "    row_major float1x4 m;    float probe;", 32);
    check_rule("row_major float2x4", "    row_major float2x4 m;    float probe;", 48);
    check_rule("row_major float3x4", "    row_major float3x4 m;    float probe;", 64);
    check_rule("row_major float4x4", "    row_major float4x4 m;    float probe;", 80);

    // Column-major, so it is the ROW count instead -- the same eight numbers, transposed.
    check_rule("column_major float4x1", "    column_major float4x1 m;    float probe;", 32);
    check_rule("column_major float4x2", "    column_major float4x2 m;    float probe;", 48);
    check_rule("column_major float4x3", "    column_major float4x3 m;    float probe;", 64);
    check_rule("column_major float4x4", "    column_major float4x4 m;    float probe;", 80);

    // And a matrix starts a whole row, so what precedes it is padded out to one.
    check_rule("a matrix starts a row", "    float a;    column_major float4x2 m;    float probe;", 64);
}

TEST("portable-hlsl spike - Q14g2 a partial last row is what SPIR-V refuses, and only with a member after it")
{
    // The minimal case, and the whole reason the table admits only full-float4 vectors:
    //
    //     struct block { row_major float2x2 m; float probe; };
    //
    // D3D lays the matrix out as two rows at a 16-byte stride with 8 bytes used each, so its extent is 24 and
    // `probe` packs into the last row's tail at 24.
    // The SPIR-V validator measures the same matrix as `MatrixStride * V` = 32, so it reads `probe` as landing
    // INSIDE the matrix and rejects the module:
    //
    //     member 1 at offset 24 overlaps previous member ending at offset 31
    //
    // The disagreement is exactly whether the last row claims its full stride.
    // D3D says no, the validator yes.
    CHECK(block_size_of("    row_major float2x2 m;    float probe;", ssc::dxc::compile_target::spirv) == -1);
    CHECK(block_size_of("    row_major float3x3 m;    float probe;", ssc::dxc::compile_target::spirv) == -1);
    CHECK(block_size_of("    row_major float4x3 m;    float probe;", ssc::dxc::compile_target::spirv) == -1);

    // It is the FOLLOWING member that is refused rather than the matrix, which is what says the tail is the
    // subject: the same matrix last in the block, with nothing to pack into it, compiles.
    CHECK(block_size_of("    float probe;\n    row_major float2x2 m;", ssc::dxc::compile_target::spirv) == 48);

    // Column-major escapes this in practice even where the arithmetic looks identical, and the SPIR-V was not
    // dumped to find out why the RowMajor decoration is measured differently.
    // It does not gate anything: the pass admits only vectors of four, where extent and stride * V coincide
    // and there is no tail to disagree about.
    check_rule("column_major float2x2 packs its tail", "    column_major float2x2 m;    float probe;", 32);
}

TEST("portable-hlsl spike - Q14h half is 32-bit storage here, and it is a compile flag that says so")
{
    // `half` and `min16float` are the same 32 bits as `float` unless `-enable-16bit-types` is passed, and
    // nothing in ssc passes it — so the pass's mirror declares a `float` for both.
    //
    // These numbers are what makes that safe rather than lucky.
    // The day anything adds that flag they go red, which is a failing test rather than a silently wrong
    // number in a constant buffer — the failure this whole spike exists to prevent.
    check_rule("half packs as float", "    half a;    float probe;", 16);
    check_rule("half2 packs as float2", "    half2 a;    float probe;", 16);
    check_rule("half3 packs as float3", "    half3 a;    float probe;", 16);
    check_rule("half4 packs as float4", "    half4 a;    float probe;", 32);
    check_rule("min16float too", "    float a;    min16float b;    float probe;", 16);
}

TEST("portable-hlsl spike - Q14i a 64-bit member aligns, and a 64-bit vector aligns to a whole row")
{
    // The 32-bit rules do not describe these, which is why they are measured separately.
    // A scalar `double` / `int64_t` starts at an 8-byte boundary; a vector of them starts a row.
    // Neither obeys the no-straddling rule: a `double3` is 24 bytes and crosses a row boundary outright.
    check_rule("double is eight bytes", "    double a;    float b;    float probe;", 16);
    check_rule("a double aligns to eight", "    float a;    double b;    float probe;", 32);
    check_rule("and to eight, not to a row", "    float3 a;    double b;    float probe;", 32);
    check_rule("a double2 starts a row", "    float a;    double2 b;    float probe;", 48);
    check_rule("a double3 straddles one", "    double3 a;    float b;    float probe;", 32);
    check_rule("a double4 is two rows", "    double4 a;    float probe;", 48);

    // The 64-bit integers are the same type family to the layout rules.
    check_rule("uint64_t aligns to eight", "    float a;    uint64_t b;    float probe;", 32);
    check_rule("an int64_t2 starts a row", "    float a;    int64_t2 b;    float probe;", 48);
    check_rule("an int64_t3 straddles one", "    int64_t3 a;    float b;    float probe;", 32);
}
