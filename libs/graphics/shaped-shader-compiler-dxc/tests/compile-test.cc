#include <nexus/test.hh>
#include <shaped-shader-compiler-dxc/all.hh>

// The compile step turns (already-preprocessed) HLSL into an sg::compiled_shader: DXIL bytecode plus
// reflected bindings and the compute workgroup size.

namespace
{
// Output[i] = i * 2 — mirrors the dx12 backend's double_compute smoke shader. One RWStructuredBuffer
// at register u0 (space 0), a 64-thread group. Self-contained (no includes).
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
    CHECK(b.set == 0u);
    CHECK(b.index == 0u);
    CHECK(b.count == 1u);

    CHECK(shader.compiler.name == cc::string_view("dxc"));
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
