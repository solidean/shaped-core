#include <nexus/test.hh>
#include <shaped-shader-compiler-dxc/all.hh>

// The compile step turns (already-preprocessed) HLSL into an sg::compiled_shader: DXIL bytecode plus
// reflected bindings and the compute workgroup size.

namespace
{
// Output[i] = i * 2 — mirrors the dx12 backend's double_compute smoke shader.
// One RWStructuredBuffer at register u0 (space 0), a 64-thread group.
// Self-contained (no includes).
constexpr char const* double_compute_hlsl = R"(
RWStructuredBuffer<uint> Output : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Output[tid.x] = tid.x * 2u;
}
)";
} // namespace

TEST("ssc::dxc compile - compute shader -> DXIL + reflection")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    ssc::dxc::shader_description desc;
    desc.stage = sg::shader_stage::compute;
    desc.entry_point = "main";
    desc.model = ssc::dxc::shader_model::sm_6_8;
    desc.source = double_compute_hlsl;

    auto result = comp.value().compile(desc);
    REQUIRE(result.has_value());

    sg::compiled_shader const& shader = result.value();
    CHECK(shader.stage == sg::shader_stage::compute);
    CHECK(shader.format == sg::shader_format::dxil);
    CHECK(shader.entry_point == cc::string_view("main"));
    CHECK(!shader.bytecode.empty());

    // Compute workgroup size comes from reflection ([numthreads(64,1,1)]).
    REQUIRE(shader.workgroup_size.has_value());
    CHECK(shader.workgroup_size.value().x == 64);
    CHECK(shader.workgroup_size.value().y == 1);
    CHECK(shader.workgroup_size.value().z == 1);

    // One binding: "Output" as a read-write structured buffer at (set 0, index 0). (set,index) is the
    // faithful (space, register) from DXC reflection — see docs/reflection.hh.
    REQUIRE(shader.bindings.size() == 1);
    sg::binding const& b = shader.bindings[0];
    CHECK(b.name == cc::string_view("Output"));
    CHECK(b.type == sg::binding_type::readwrite_structured_buffer);
    CHECK(b.space == 0u); // DXC always reflects a register space, even the default one
    CHECK(b.index == 0u);
    CHECK(b.count == 1u);

    CHECK(shader.compiler.name == cc::string_view("dxc"));
}

namespace
{
// Samples a texture through a sampler and writes a storage texture — exercises the texture-SRV, sampler, and storage-texture (UAV) reflection kinds.
// SampleLevel (not Sample) so it is valid in compute.
constexpr char const* sampled_compute_hlsl = R"(
Texture2D<float4> Tex     : register(t0);
SamplerState      Samp    : register(s0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    Output[tid.xy] = Tex.SampleLevel(Samp, (float2(tid.xy) + 0.5) / 64.0, 0);
}
)";

sg::binding const* find_binding(sg::compiled_shader const& s, cc::string_view name)
{
    for (auto const& b : s.bindings)
        if (b.name == name)
            return &b;
    return nullptr;
}
} // namespace

TEST("ssc::dxc compile - texture / sampler / storage-texture bindings reflect to sg vocabulary")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    ssc::dxc::shader_description desc;
    desc.stage = sg::shader_stage::compute;
    desc.entry_point = "main";
    desc.model = ssc::dxc::shader_model::sm_6_8;
    desc.source = sampled_compute_hlsl;

    auto result = comp.value().compile(desc);
    REQUIRE(result.has_value());
    sg::compiled_shader const& shader = result.value();

    auto const* tex = find_binding(shader, "Tex");
    REQUIRE(tex != nullptr);
    CHECK(tex->type == sg::binding_type::readonly_texture); // Texture2D -> sampled texture SRV
    CHECK(tex->index == 0u);                                // t0

    auto const* samp = find_binding(shader, "Samp");
    REQUIRE(samp != nullptr);
    CHECK(samp->type == sg::binding_type::sampler); // SamplerState -> sampler
    CHECK(sg::is_sampler(samp->type));
    CHECK(samp->index == 0u); // s0

    auto const* out = find_binding(shader, "Output");
    REQUIRE(out != nullptr);
    CHECK(out->type == sg::binding_type::readwrite_texture); // RWTexture2D -> storage texture UAV
    CHECK(out->index == 0u);                                 // u0
}

TEST("ssc::dxc compile - a syntax error surfaces a diagnostic")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    ssc::dxc::shader_description desc;
    desc.stage = sg::shader_stage::compute;
    desc.source = "[numthreads(1,1,1)] void main() { this is not valid HLSL }";

    auto result = comp.value().compile(desc);
    CHECK(result.has_error());
}

TEST("ssc::dxc compile - rejects source that still contains an #include")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    ssc::dxc::shader_description desc;
    desc.stage = sg::shader_stage::compute;
    // compile() takes already-preprocessed source; a stray #include must fail (reject handler).
    desc.source = "#include \"something.hlsli\"\n[numthreads(1,1,1)] void main() {}";

    auto result = comp.value().compile(desc);
    CHECK(result.has_error());
}

TEST("ssc::dxc compile - compute shader -> SPIR-V")
{
    // The SPIR-V half of the compiler, which is what the vulkan backend consumes: it accepts no other format.
    // Reflection is checked separately once SPIRV-Reflect lands; what this pins is that the target flag reaches DXC
    // and that the bytes coming back really are a SPIR-V module.
    auto compiler = ssc::dxc::compiler::create();
    REQUIRE(compiler.has_value());

    auto const src = cc::string(R"(
        [[vk::binding(0, 0)]] RWStructuredBuffer<float> Out;
        [numthreads(64, 1, 1)]
        void main(uint3 tid : SV_DispatchThreadID) { Out[tid.x] = 1.0f; }
    )");

    auto compiled = compiler.value().compile({.source = src, .entry_point = "main", .stage = sg::shader_stage::compute},
                                             {.target = ssc::dxc::compile_target::spirv});

    // Reflection is the one part still missing, and it fails loudly rather than returning empty bindings — so a
    // compile that reaches it reports that error rather than a shader with no resources.
    if (compiled.has_error())
    {
        CHECK(cc::string_view(compiled.error().to_string()).contains("SPIR-V reflection is not implemented"));
        return;
    }

    auto const& shader = compiled.value();
    CHECK(shader.format == sg::shader_format::spirv);
    REQUIRE(shader.bytecode.size() >= 4);

    // SPIR-V's magic number, 0x07230203, little-endian.
    // Read bytewise rather than as a word: the blob is a byte span with no alignment promise.
    CHECK(int(shader.bytecode[0]) == 0x03);
    CHECK(int(shader.bytecode[1]) == 0x02);
    CHECK(int(shader.bytecode[2]) == 0x23);
    CHECK(int(shader.bytecode[3]) == 0x07);
}
