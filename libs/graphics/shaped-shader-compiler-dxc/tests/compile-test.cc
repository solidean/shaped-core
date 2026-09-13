#include <nexus/test.hh>
#include <shaped-shader-compiler-dxc/all.hh>

using namespace cc::primitive_defines;

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

// DXIL reflection reads a container beside the bytecode through the Windows SDK's d3d12shader.h, which the Linux DXC
// release does not ship — so the tests that assert reflected bindings from a DXIL compile are Windows-only.
// The SPIR-V test at the end of this file is the cross-platform counterpart.
#ifdef CC_OS_WINDOWS
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

#endif // CC_OS_WINDOWS

TEST("ssc::dxc compile - the SPIR-V flag set applies to every stage")
{
    // The SPIR-V arm carries four -fvk-* flags that align it with DXIL's behaviour (see build_compile_args).
    // Some of DXC's -fvk-* flags are stage-restricted — `-fvk-invert-y` is VS/DS/GS/MS/Lib only — so a flag added for
    // one stage can reject another, and compute alone would not notice.
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());
    auto& c = comp.value();

    auto const raster_src = cc::string(R"(
        struct vs_out { float4 pos : SV_Position; float4 color : COLOR; };
        vs_out main_vs(float2 p : POSITION) { vs_out o; o.pos = float4(p, 0, 1); o.color = 1; return o; }
        float4 main_ps(vs_out i) : SV_Target { return i.color / i.pos.w; }
    )");

    // main_ps reads SV_Position.w, which is what -fvk-use-dx-position-w exists for.
    auto vs = c.compile({.source = raster_src, .entry_point = "main_vs", .stage = sg::shader_stage::vertex},
                        {.target = ssc::dxc::compile_target::spirv});
    auto ps = c.compile({.source = raster_src, .entry_point = "main_ps", .stage = sg::shader_stage::fragment},
                        {.target = ssc::dxc::compile_target::spirv});
    CHECK(vs.has_value());
    CHECK(ps.has_value());

    auto const rt_src = cc::string(R"(
        RWTexture2D<float4> Output;
        [shader("raygeneration")]
        void main_rg() { Output[DispatchRaysIndex().xy] = float4(1, 0, 0, 1); }
    )");
    auto rg = c.compile({.source = rt_src, .entry_point = "main_rg", .stage = sg::shader_stage::raygen},
                        {.target = ssc::dxc::compile_target::spirv});
    CHECK(rg.has_value());
}

namespace
{
/// One word of a SPIR-V module, read bytewise because the blob carries no alignment promise.
[[nodiscard]] unsigned spirv_word_at(sg::compiled_shader const& shader, isize word)
{
    auto const i = word * 4;
    return unsigned(shader.bytecode[i]) | (unsigned(shader.bytecode[i + 1]) << 8)
         | (unsigned(shader.bytecode[i + 2]) << 16) | (unsigned(shader.bytecode[i + 3]) << 24);
}

/// Whether the module contains an instruction with this opcode, and with this first operand where one is given.
///
/// A SPIR-V module is a five-word header followed by instructions whose first word packs the word count in the
/// high half and the opcode in the low.
/// Walking one therefore needs no library and no device, which is what makes the flags below checkable at all:
/// reflection reports none of them, and the vulkan backend's own tests compile no HLSL.
[[nodiscard]] bool spirv_contains(sg::compiled_shader const& shader,
                                  unsigned opcode,
                                  cc::optional<unsigned> first_operand = {})
{
    auto const words = shader.bytecode.size() / 4;
    auto at = isize(5);
    while (at < words)
    {
        auto const header = spirv_word_at(shader, at);
        auto const count = isize(header >> 16);
        if (count <= 0)
            return false; // malformed rather than merely unexpected — stop instead of looping forever

        if ((header & 0xFFFFu) == opcode
            && (!first_operand.has_value() || (count > 1 && spirv_word_at(shader, at + 1) == first_operand.value())))
            return true;
        at += count;
    }
    return false;
}

// What these tests look for, as SPIR-V's own tables number them.
//
// The CAPABILITY is what to check rather than the `SPV_KHR_shader_draw_parameters` extension: ssc targets
// vulkan1.3, where draw parameters are core and no `OpExtension` is emitted at all.
constexpr unsigned k_op_capability = 17;
constexpr unsigned k_op_isub = 130;
constexpr unsigned k_op_fdiv = 136;
constexpr unsigned k_capability_draw_parameters = 4427;

[[nodiscard]] sg::compiled_shader compile_spirv(ssc::dxc::compiler& c,
                                                cc::string_view src,
                                                cc::string_view entry,
                                                sg::shader_stage stage)
{
    auto r = c.compile(
        {.source = cc::string::create_copy_of(src), .entry_point = cc::string::create_copy_of(entry), .stage = stage},
        {.target = ssc::dxc::compile_target::spirv});
    REQUIRE(r.has_value());
    return cc::move(r.value());
}
} // namespace

TEST("ssc::dxc compile - SV_VertexID and SV_InstanceID exclude the base, as they do on DXIL")
{
    // Vulkan's VertexIndex INCLUDES the draw's base vertex where D3D's SV_VertexID does not, and the same for
    // InstanceIndex against SV_InstanceID.
    // So without `-fvk-support-nonzero-base-vertex` / `-fvk-support-nonzero-base-instance` one source indexes a
    // vertex buffer differently on the two backends the moment a draw has a non-zero base — and nothing reports
    // it, because both modules compile and both pipelines run.
    //
    // The flags make DXC read the base as a builtin and subtract it, and that is what is checked here: nothing
    // else can see them, since reflection reports no flag and the vulkan backend's tests compile no HLSL.
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());
    auto& c = comp.value();

    auto const reads_base = cc::string_view(R"(
        struct vs_out { float4 pos : SV_Position; uint id : TEXCOORD0; };
        vs_out main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
        {
            vs_out o;
            o.pos = float4(float(vid), float(iid), 0, 1);
            o.id = vid + iid;
            return o;
        }
    )");

    auto const shader = compile_spirv(c, reads_base, "main", sg::shader_stage::vertex);

    // BaseVertex and BaseInstance are draw-parameter builtins, so reading them declares that capability...
    CHECK(spirv_contains(shader, k_op_capability, k_capability_draw_parameters));

    // ...and the subtraction that turns an index into an id is an OpISub, one per semantic.
    CHECK(spirv_contains(shader, k_op_isub));

    // The control, and what makes the two above the flags' doing rather than something every vertex shader
    // carries: the same stage reading neither semantic needs neither.
    auto const reads_neither = cc::string_view(R"(
        float4 main(float2 p : POSITION) : SV_Position { return float4(p, 0, 1); }
    )");

    auto const plain = compile_spirv(c, reads_neither, "main", sg::shader_stage::vertex);
    CHECK(!spirv_contains(plain, k_op_capability, k_capability_draw_parameters));
    CHECK(!spirv_contains(plain, k_op_isub));
}

TEST("ssc::dxc compile - SV_Position.w is the clip w, not the reciprocal FragCoord carries")
{
    // Vulkan's FragCoord.w is 1/w where D3D's SV_Position.w is w itself.
    // A shader dividing by it would therefore be multiplying by it on the other backend — a wrong picture rather
    // than a failure, which is the class this whole flag set exists to close.
    //
    // `-fvk-use-dx-position-w` makes DXC take the reciprocal back, and that reciprocal is the OpFDiv below.
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());
    auto& c = comp.value();

    auto const reads_w = cc::string_view(R"(
        float4 main(float4 pos : SV_Position) : SV_Target { return float4(pos.w, 0, 0, 1); }
    )");

    auto const shader = compile_spirv(c, reads_w, "main", sg::shader_stage::fragment);
    CHECK(spirv_contains(shader, k_op_fdiv));

    // The control: the same stage reading .xy divides by nothing, so the OpFDiv above is the flag's and not
    // something a fragment shader always has.
    auto const reads_xy = cc::string_view(R"(
        float4 main(float4 pos : SV_Position) : SV_Target { return float4(pos.xy, 0, 1); }
    )");

    auto const plain = compile_spirv(c, reads_xy, "main", sg::shader_stage::fragment);
    CHECK(!spirv_contains(plain, k_op_fdiv));
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

    REQUIRE(compiled.has_value());
    auto const& shader = compiled.value();
    CHECK(shader.format == sg::shader_format::spirv);
    REQUIRE(shader.bytecode.size() >= 4);

    // SPIR-V's magic number, 0x07230203, little-endian.
    // Read bytewise rather than as a word: the blob is a byte span with no alignment promise.
    CHECK(int(shader.bytecode[0]) == 0x03);
    CHECK(int(shader.bytecode[1]) == 0x02);
    CHECK(int(shader.bytecode[2]) == 0x23);
    CHECK(int(shader.bytecode[3]) == 0x07);

    // Reflection comes out of the module itself here, not a container beside it.
    REQUIRE(shader.bindings.size() == 1);
    auto const& b = shader.bindings[0];
    CHECK(b.name == "Out");
    CHECK(b.index == 0);
    CHECK(b.type == sg::binding_type::readwrite_structured_buffer);

    // The set fills group_index and `space` stays absent, which is the opposite of what the DXIL arm reports for the
    // same source, and what a vulkan group layout needs.
    REQUIRE(b.group_index.has_value());
    CHECK(b.group_index.value() == 0);
    CHECK(!b.space.has_value());

    REQUIRE(shader.workgroup_size.has_value());
    CHECK(shader.workgroup_size.value().x == 64);
    CHECK(shader.workgroup_size.value().y == 1);
    CHECK(shader.workgroup_size.value().z == 1);
}
