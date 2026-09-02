#include <clean-core/common/log.hh>
#include <clean-core/string/format.hh>
#include <nexus/test.hh>
#include <shaped-shader-compiler-dxc/all.hh>

// The DXC behaviours a portable-HLSL prelude rests on, pinned here so a DXC upgrade that changes one of them fails a
// test rather than a shader.
//
// What these answer, and what each answer decided:
//   1. an unguarded `[[vk::...]]` is `-Wignored-attributes` on the DXIL target, which -WX turns into an error — so
//      every annotation forks on `__spirv__`, inside the prelude, once.
//   2. `__COUNTER__` works and survives the pasting indirection, so a binding's index need not be written by hand.
//   3. a duplicate file-scope `static const` is a redefinition error, so a pasted marker symbol makes two bindings
//      claiming one slot a compile error; an unused marker is quiet under -WX.
//   4. DXC assigns DXIL registers per class from zero when the source names none, and a SPIR-V set takes sparse
//      indices — so neither target needs the numbers spelled out.
//   5. across the two targets a binding keeps its name and type but NOT its index, which is why binding groups match
//      by name and why an equivalence check may not compare indices.
//   6. a macro cannot emit a preprocessor directive, so a group cannot be opened by a macro call — which is why the
//      group is an argument on each binding rather than a block around a run of them.
//
// Every compile runs with default compile_options, so warnings_as_errors is ON — that is the point of (1).

namespace
{
[[nodiscard]] cc::result<sg::compiled_shader> compile_as(ssc::dxc::compiler& c,
                                                         cc::string_view src,
                                                         sg::shader_stage stage,
                                                         cc::string_view entry,
                                                         ssc::dxc::compile_target target)
{
    return c.compile({.source = cc::string(src), .entry_point = cc::string(entry), .stage = stage}, {.target = target});
}

/// Compiles and reports the DXC diagnostic either way, so a run says *what* DXC thinks rather than only that it agreed.
[[nodiscard]] bool compiles(ssc::dxc::compiler& c,
                            cc::string_view what,
                            cc::string_view src,
                            sg::shader_stage stage,
                            cc::string_view entry,
                            ssc::dxc::compile_target target)
{
    auto result = compile_as(c, src, stage, entry, target);
    if (result.has_error())
    {
        CC_LOG_INFO("[spike] {} rejected: {}", what, result.error().to_string());
        return false;
    }
    return true;
}

[[nodiscard]] sg::binding const* find_binding(sg::compiled_shader const& s, cc::string_view name)
{
    for (auto const& b : s.bindings)
        if (b.name == name)
            return &b;
    return nullptr;
}

// --- Q1: one vk:: attribute per snippet, so a failure names which attribute DXC objects to ---

constexpr char const* vk_binding_hlsl = R"(
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) { Out[tid.x] = 1u; }
)";

constexpr char const* vk_push_constant_hlsl = R"(
struct spike_constants { uint scale; };
[[vk::push_constant]] ConstantBuffer<spike_constants> Push : register(b0);
RWStructuredBuffer<uint> Out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) { Out[tid.x] = tid.x * Push.scale; }
)";

constexpr char const* vk_location_hlsl = R"(
struct vs_input
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
};

float4 main_vs(vs_input v) : SV_Position { return float4(v.position + v.normal * 0.0f, 1.0f); }
)";

// --- The prelude shape Q1 forces, written out in full ---
//
// `SC_BINDING(group)` is prefix-only: it emits a marker symbol naming the slot it took, plus the SPIR-V annotation
// where there is one, and leaves the declaration itself to the author.
// The index comes from __COUNTER__ through two levels of indirection, so it is a plain number by the time it reaches
// the ## in SC_CAT — and it is expanded exactly once, or the annotation and the marker would disagree.
//
// The `__spirv__` fork lives here and nowhere else, which is the whole reason for a prelude: DXIL rejects the
// annotation outright, so an author who writes one by hand ships a shader that only builds on one backend.
constexpr char const* prelude_macros = R"(
#define SC_CAT_(a, b) a##b
#define SC_CAT(a, b) SC_CAT_(a, b)

#ifdef __spirv__
#define SC_ANNOTATE(group, index) [[vk::binding(index, group)]]
#else
#define SC_ANNOTATE(group, index)
#endif

#define SC_BINDING(group) SC_BINDING_AT(group, __COUNTER__)
#define SC_BINDING_AT(group, index) SC_BINDING_I(group, index)
#define SC_BINDING_I(group, index) \
    static const uint SC_CAT(SC_CAT(SC_CAT(sc_slot_taken_, group), _), index) = uint(group); \
    SC_ANNOTATE(group, index)
)";

/// The macros plus a body — every snippet below is "prelude + declarations + entry point".
[[nodiscard]] cc::string with_prelude(cc::string_view body)
{
    return cc::format("{}{}", prelude_macros, body);
}

constexpr char const* counter_body = R"(
SC_BINDING(0) Texture2D<float4> Albedo;
SC_BINDING(0) SamplerState LinearSampler;
SC_BINDING(0) RWTexture2D<float4> Output;

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Output[tid.xy] = Albedo.SampleLevel(LinearSampler, (float2(tid.xy) + 0.5f) / 64.0f, 0);
}
)";

// Two bindings pinned to one slot: the marker symbols collide, which is the compile-time half of the validation story.
// Without the markers this compiles clean and the collision only surfaces at pipeline creation, if at all.
constexpr char const* duplicate_slot_body = R"(
SC_BINDING_AT(0, 3) RWStructuredBuffer<uint> A;
SC_BINDING_AT(0, 3) RWStructuredBuffer<uint> B;

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) { A[tid.x] = B[tid.x]; }
)";

// --- Q7: could a group be opened by a macro pair rather than a #define? ---
//
// A macro cannot emit a preprocessor directive, so SG_BEGIN_BINDING_GROUP(n) cannot #define anything.
// The only remaining carrier is an HLSL constant the annotation reads, which needs [[vk::binding]] to take a
// constant expression rather than an integer literal.
constexpr char const* constant_expression_binding_hlsl = R"(
static const uint sc_group = 1;
[[vk::binding(0, sc_group)]] RWStructuredBuffer<uint> Out;

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) { Out[tid.x] = 1u; }
)";

// An object-like macro whose body looks like a directive.
// The tokens it expands to are not rescanned as one, so SC_GROUP never becomes defined — the `#` and `define` just
// land in the token stream where HLSL has to parse them.
constexpr char const* macro_emitting_define_hlsl = R"(
#define OPEN_GROUP #define SC_GROUP 0
OPEN_GROUP
RWStructuredBuffer<uint> Out;

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) { Out[tid.x] = SC_GROUP; }
)";

// --- Q4 ---

// No register() anywhere: DXC assigns them, and reflection is what sg reads back.
constexpr char const* auto_register_hlsl = R"(
Texture2D<float4> TexA;
Texture2D<float4> TexB;
SamplerState Samp;
RWTexture2D<float4> Out;

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    float2 uv = (float2(tid.xy) + 0.5f) / 64.0f;
    Out[tid.xy] = TexA.SampleLevel(Samp, uv, 0) + TexB.SampleLevel(Samp, uv, 0);
}
)";

// Indices 0 and 5 in one set, which is what per-file __COUNTER__ numbering produces once several groups interleave.
constexpr char const* sparse_index_hlsl = R"(
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> First;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> Second;

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) { First[tid.x] = Second[tid.x]; }
)";
} // namespace

// DXIL reflection reads the container beside the bytecode through the Windows SDK's d3d12shader.h, which the Linux
// DXC release does not ship — so every DXIL-target case here is Windows-only, as in compile-test.cc.
#ifdef CC_OS_WINDOWS

TEST("portable-hlsl spike - Q1 vk:: attributes are rejected on the DXIL target under -WX")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());
    auto& c = comp.value();

    // All three are `'<attr>' attribute ignored [-Werror,-Wignored-attributes]`, so an unguarded annotation is a hard
    // error on DXIL rather than a no-op.
    // This is what makes the `__spirv__` fork mandatory, and what makes writing one by hand a trap worth a macro.
    CHECK(!compiles(c, "[[vk::binding]]", vk_binding_hlsl, sg::shader_stage::compute, "main",
                    ssc::dxc::compile_target::dxil));
    CHECK(!compiles(c, "[[vk::push_constant]]", vk_push_constant_hlsl, sg::shader_stage::compute, "main",
                    ssc::dxc::compile_target::dxil));
    CHECK(!compiles(c, "[[vk::location]]", vk_location_hlsl, sg::shader_stage::vertex, "main_vs",
                    ssc::dxc::compile_target::dxil));

    // The same sources are fine on the SPIR-V target, which is what makes the fork a fork rather than a ban.
    CHECK(compiles(c, "[[vk::binding]] (spirv)", vk_binding_hlsl, sg::shader_stage::compute, "main",
                   ssc::dxc::compile_target::spirv));
}

TEST("portable-hlsl spike - Q4 DXC assigns registers and reflection reports them")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto result = compile_as(comp.value(), auto_register_hlsl, sg::shader_stage::compute, "main",
                             ssc::dxc::compile_target::dxil);
    if (result.has_error())
        CC_LOG_INFO("[spike] auto-assigned registers rejected: {}", result.error().to_string());
    REQUIRE(result.has_value());

    // Each register class numbers from zero independently, so the numbers a shader never wrote are still the numbers
    // sg binds against — TexA/TexB take t0/t1 while Out takes u0.
    auto const& shader = result.value();
    auto const* tex_a = find_binding(shader, "TexA");
    auto const* tex_b = find_binding(shader, "TexB");
    auto const* out = find_binding(shader, "Out");
    REQUIRE(tex_a != nullptr);
    REQUIRE(tex_b != nullptr);
    REQUIRE(out != nullptr);
    CHECK(tex_a->index == 0u);
    CHECK(tex_b->index == 1u);
    CHECK(out->index == 0u);
}

TEST("portable-hlsl spike - Q5 one source, two targets, matching names and types")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());
    auto& c = comp.value();

    auto const src = with_prelude(counter_body);
    auto dxil = compile_as(c, src, sg::shader_stage::compute, "main", ssc::dxc::compile_target::dxil);
    auto spirv = compile_as(c, src, sg::shader_stage::compute, "main", ssc::dxc::compile_target::spirv);
    if (dxil.has_error())
        CC_LOG_INFO("[spike] prelude rejected on dxil: {}", dxil.error().to_string());
    if (spirv.has_error())
        CC_LOG_INFO("[spike] prelude rejected on spirv: {}", spirv.error().to_string());
    REQUIRE(dxil.has_value());
    REQUIRE(spirv.has_value());

    // Same declarations, so the same three bindings with the same kinds — this is the equivalence a cross-target
    // check would assert for a whole package.
    REQUIRE(dxil.value().bindings.size() == spirv.value().bindings.size());
    for (auto const& b : spirv.value().bindings)
    {
        auto const* other = find_binding(dxil.value(), b.name);
        REQUIRE(other != nullptr);
        CHECK(other->type == b.type);
    }

    // The indices do NOT agree, and are not meant to: SPIR-V takes the counter's number while DXIL takes DXC's per-class assignment.
    // A binding group resolves a name, so this is a divergence sg is built to tolerate — but it is why an equivalence check compares names and kinds rather than addresses.
    auto const* spirv_output = find_binding(spirv.value(), "Output");
    auto const* dxil_output = find_binding(dxil.value(), "Output");
    REQUIRE(spirv_output != nullptr);
    REQUIRE(dxil_output != nullptr);
    CHECK(spirv_output->index == 2u);
    CHECK(dxil_output->index == 0u);
}

#endif

TEST("portable-hlsl spike - Q2 __COUNTER__ survives the macro indirection")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto const src = with_prelude(counter_body);
    auto result = compile_as(comp.value(), src, sg::shader_stage::compute, "main", ssc::dxc::compile_target::spirv);
    if (result.has_error())
        CC_LOG_INFO("[spike] __COUNTER__ prelude rejected: {}", result.error().to_string());
    REQUIRE(result.has_value());

    // Three declarations in declaration order, so the counter produced 0, 1, 2 — all in set 0, none written by hand.
    auto const& shader = result.value();
    auto const* albedo = find_binding(shader, "Albedo");
    auto const* sampler = find_binding(shader, "LinearSampler");
    auto const* output = find_binding(shader, "Output");
    REQUIRE(albedo != nullptr);
    REQUIRE(sampler != nullptr);
    REQUIRE(output != nullptr);
    CHECK(albedo->index == 0u);
    CHECK(sampler->index == 1u);
    CHECK(output->index == 2u);
    REQUIRE(albedo->group_index.has_value());
    CHECK(albedo->group_index.value() == 0u);
}

TEST("portable-hlsl spike - Q3 a duplicate slot marker is a compile error")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    // The unused-marker half is covered by Q2: it compiles under -WX with three markers nothing reads.
    auto const src = with_prelude(duplicate_slot_body);
    auto result = compile_as(comp.value(), src, sg::shader_stage::compute, "main", ssc::dxc::compile_target::spirv);
    if (result.has_value())
        CC_LOG_INFO("[spike] duplicate slot markers compiled — HLSL tolerates the redefinition");
    else
        CC_LOG_INFO("[spike] duplicate slot markers rejected: {}", result.error().to_string());
    CHECK(result.has_error());
}

TEST("portable-hlsl spike - Q7 [[vk::binding]] takes a constant expression, and it still is not enough")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto result = compile_as(comp.value(), constant_expression_binding_hlsl, sg::shader_stage::compute, "main",
                             ssc::dxc::compile_target::spirv);
    if (result.has_error())
        CC_LOG_INFO("[spike] constant-expression binding rejected: {}", result.error().to_string());
    REQUIRE(result.has_value());

    // The annotation reads an HLSL constant, so a group *could* be carried by one rather than by a macro.
    REQUIRE(result.value().bindings[0].group_index.has_value());
    CHECK(result.value().bindings[0].group_index.value() == 1u);

    // What this does not buy is a SG_BEGIN_BINDING_GROUP / SG_END_BINDING_GROUP pair.
    // The carrier has to have one fixed name for the binding macro to read, an END cannot undeclare it, and a second
    // BEGIN in the same file redeclares it — so a macro pair could only ever serve one group per file.
    // That is why the group is an argument on the binding instead.
}

TEST("portable-hlsl spike - Q7b a macro cannot emit a #define")
{
    // The reason a group cannot be opened by a macro call, demonstrated rather than asserted.
    // A macro's replacement list is rescanned for further macros, never for directives, so this defines nothing.
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    CHECK(!compiles(comp.value(), "a macro expanding to #define", macro_emitting_define_hlsl, sg::shader_stage::compute,
                    "main", ssc::dxc::compile_target::spirv));
}

TEST("portable-hlsl spike - Q4 sparse indices within one SPIR-V set")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    auto result = compile_as(comp.value(), sparse_index_hlsl, sg::shader_stage::compute, "main",
                             ssc::dxc::compile_target::spirv);
    if (result.has_error())
        CC_LOG_INFO("[spike] sparse set indices rejected: {}", result.error().to_string());
    REQUIRE(result.has_value());

    auto const& shader = result.value();
    auto const* first = find_binding(shader, "First");
    auto const* second = find_binding(shader, "Second");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->index == 0u);
    CHECK(second->index == 5u);
    CHECK(first->group_index.value() == 0u);
    CHECK(second->group_index.value() == 0u);
}
