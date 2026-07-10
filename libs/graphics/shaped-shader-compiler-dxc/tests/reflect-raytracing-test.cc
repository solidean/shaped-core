#include <nexus/test.hh>
#include <shaped-shader-compiler-dxc/all.hh>

// Ray-tracing shaders compile to a single-entry DXIL library (`lib_6_x`) and reflect through
// ID3D12LibraryReflection rather than ID3D12ShaderReflection. This is device-free: it exercises the
// compile + library-reflection path only (no ID3D12Device), so it runs everywhere the compiler does.

namespace
{
// A raygen shader binding an acceleration structure (t0) and an output buffer (u0). Traces one ray and
// stores the hit color — enough to force `scene` and `Output` into the reflected bindings.
constexpr char const* raygen_hlsl = R"(
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> Output : register(u0);

struct Payload { float4 color; };

[shader("raygeneration")]
void RayGenMain()
{
    RayDesc ray;
    ray.Origin = float3(0, 0, 0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0.0;
    ray.TMax = 1000.0;

    Payload payload;
    payload.color = float4(0, 0, 0, 0);
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    Output[0] = uint(payload.color.x);
}
)";

// A closest-hit shader with no resource bindings — reflects to an empty binding list through the same
// library path, which must still locate the entry-point function by its mangled name.
constexpr char const* closest_hit_hlsl = R"(
struct Payload { float4 color; };
struct Attributes { float2 bary; };

[shader("closesthit")]
void ClosestHitMain(inout Payload payload, in Attributes attribs)
{
    payload.color = float4(1, 0, 0, 1);
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

TEST("ssc::dxc compile - raygen shader -> DXIL library + reflection")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    ssc::dxc::shader_description desc;
    desc.stage = sg::shader_stage::raygen;
    desc.entry_point = "RayGenMain";
    desc.model = ssc::dxc::shader_model::sm_6_3;
    desc.source = raygen_hlsl;

    auto result = comp.value().compile(desc);
    REQUIRE(result.has_value());

    sg::compiled_shader const& shader = result.value();
    CHECK(shader.stage == sg::shader_stage::raygen);
    CHECK(shader.format == sg::shader_format::dxil);
    CHECK(shader.entry_point == cc::string_view("RayGenMain"));
    CHECK(!shader.bytecode.empty());

    // No workgroup size for a ray-tracing stage.
    CHECK(!shader.workgroup_size.has_value());

    // The acceleration structure reflects through the library path (part-1 mapping, library reflection).
    auto const* scene = find_binding(shader, "scene");
    REQUIRE(scene != nullptr);
    CHECK(scene->type == sg::binding_type::acceleration_structure);
    CHECK(scene->index == 0u); // t0

    auto const* out = find_binding(shader, "Output");
    REQUIRE(out != nullptr);
    CHECK(out->type == sg::binding_type::readwrite_structured_buffer);
    CHECK(out->index == 0u); // u0
}

TEST("ssc::dxc compile - closest-hit shader reflects with no bindings")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    ssc::dxc::shader_description desc;
    desc.stage = sg::shader_stage::closest_hit;
    desc.entry_point = "ClosestHitMain";
    desc.model = ssc::dxc::shader_model::sm_6_3;
    desc.source = closest_hit_hlsl;

    auto result = comp.value().compile(desc);
    REQUIRE(result.has_value());

    sg::compiled_shader const& shader = result.value();
    CHECK(shader.stage == sg::shader_stage::closest_hit);
    CHECK(shader.format == sg::shader_format::dxil);
    CHECK(!shader.workgroup_size.has_value());
    CHECK(shader.bindings.empty());
}

TEST("ssc::dxc compile - a ray-tracing stage below shader model 6.3 is rejected")
{
    auto comp = ssc::dxc::compiler::create();
    REQUIRE(comp.has_value());

    ssc::dxc::shader_description desc;
    desc.stage = sg::shader_stage::raygen;
    desc.entry_point = "RayGenMain";
    desc.model = ssc::dxc::shader_model::sm_6_0; // too low for DXR
    desc.source = raygen_hlsl;

    auto result = comp.value().compile(desc);
    CHECK(result.has_error());
}
