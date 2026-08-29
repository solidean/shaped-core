#include <shaped-shader-library/compiler/dxc_compiler.hh>

// sv's material shaders are generated for its DXR path and compiled to DXIL, whose reflection reads a container
// beside the bytecode through the Windows SDK's d3d12shader.h — a header the Linux DXC release does not ship.
// So these need Windows as well as a DXC build, unlike the rest of the viewer suite.
#if SLIB_HAS_DXC && defined(CC_OS_WINDOWS)

#include "viewer_test_env.hh"

#include <clean-core/container/array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <nexus/test.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/resolve.hh>
#include <shaped-viewer/material/shader_generator.hh>
#include <shaped-viewer/resources/bindless_tables.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

// The generated material shaders, put through a real compiler.
//
// material-shader-generator-test checks the TEXT; this checks that the text is valid HLSL — that the bindless declarations, the ByteAddressBuffer loads and the sampling all say what DXC accepts.
// Those are exactly the mistakes reading the generated source does not catch.
//
// Compiled as COMPUTE rather than as a hit shader: a material fragment is stage-agnostic, and a compute entry needs no ray tracing pipeline to be a real compile.
// The test appends that entry itself.

namespace
{
/// A compute entry that reaches the generated function the way a closest-hit will: through an sv_instance read out of the
/// instance table, so `sv_make_context` and the byte layout it walks are compiled too rather than only the material itself.
constexpr cc::string_view test_entry = R"hlsl(
StructuredBuffer<sv_instance> sv_test_instances : register(t0, space0);
RWStructuredBuffer<float4> sv_test_out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    sv_instance inst = sv_test_instances[tid.x];
    sv_shading_context ctx = sv_make_context(inst, gBindlessBuffers[NonUniformResourceIndex(inst.indices)], 0, float2(0.25, 0.25));
    sv_surface s = sv_evaluate_material(ctx);
    sv_test_out[0] = float4(s.albedo, s.roughness) + float4(s.emissive, s.metallic) + float4(s.normal, s.occlusion);
}
)hlsl";

[[nodiscard]] sv::mesh make_mesh()
{
    auto const positions = cc::array<tg::pos3f>{tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0)};
    return {.name = "tri", .geometry = sv::triangle_geometry::create_from_positions(positions)};
}

[[nodiscard]] sv::mesh_attribute make_uvs(sv::attribute_frequency f = sv::attribute_frequency::per_vertex)
{
    auto const uvs = cc::array<tg::vec2f>{tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1)};
    return sv::mesh_attribute::create("uv", f, uvs);
}

/// Compiles one resolved material and fails the test with DXC's own message when it does not build.
void check_compiles(sv::resolved_material const& r)
{
    auto const& lib = *sv_test::shared_env().lib;

    auto const g = sv::generate_material_shader(r);
    auto const source = cc::format("{}\n{}", g.source, test_entry);

    // The runtime include lives in sv's own shader package, so its mount is what resolves it.
    auto const shader = lib.compile_source(source, sg::shader_stage::compute, "main", sg::shader_format::dxil,
                                           {.include_dir = "sv_shaders", .label = "<generated material>"});

    REQUIRE(shader != nullptr);
    (void)cc::try_async_blocking_get(shader);
    if (shader->has_error())
        FAIL(cc::format("{}\n--- source ---\n{}", shader->try_error()->underlying().to_string(), source));
    REQUIRE(shader->has_value());
    CHECK(shader->try_value()->bytecode.size() > 0);
}

} // namespace

TEST("sv - every builtin material type compiles at its default frequencies")
{
    if (!sv_test::shared_env().has_compiler)
        return; // no DXC installed: nothing here can compile

    auto materials = sv::material_library::create();
    sv::register_builtin_material_types(materials);

    for (auto const& name : {sv::builtin_material::pbr, sv::builtin_material::unlit})
    {
        auto const type = materials.acquire_type(name).value();
        auto const id = materials.acquire(sv::material::create(cc::string(name), type, {}));
        check_compiles(sv::resolve_material(materials, id, make_mesh()));
    }
}

TEST("sv - a permutation compiles at every attribute frequency")
{
    if (!sv_test::shared_env().has_compiler)
        return;

    auto materials = sv::material_library::create();
    sv::register_builtin_material_types(materials);
    auto const pbr = materials.acquire_type(sv::builtin_material::pbr).value();
    auto const gold = materials.acquire(sv::material::create("gold", pbr, {}));

    auto const scalars = cc::array<f32>{0.1f, 0.2f, 0.3f};
    auto const colors = cc::array<tg::vec3f>{tg::vec3f(1, 0, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, 1)};

    // A per-vertex vector and a per-triangle scalar together, so the interpolated and the flat load are both in one shader.
    auto mesh = make_mesh();
    mesh.attributes.push_back(sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_vertex, colors));
    mesh.attributes.push_back(
        sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_triangle, cc::array<f32>{0.4f}));
    mesh.attributes.push_back(sv::mesh_attribute::create("metallic", sv::attribute_frequency::per_corner, scalars));
    mesh.attributes.push_back(sv::mesh_attribute::create_value("occlusion", 0.5f));
    check_compiles(sv::resolve_material(materials, gold, mesh));
}

TEST("sv - a sampled permutation compiles, at either uv frequency")
{
    if (!sv_test::shared_env().has_compiler)
        return;

    auto materials = sv::material_library::create();
    sv::register_builtin_material_types(materials);
    auto const pbr = materials.acquire_type(sv::builtin_material::pbr).value();
    auto const gold = materials.acquire(sv::material::create("gold", pbr, {}));

    for (auto const frequency : {sv::attribute_frequency::per_vertex, sv::attribute_frequency::per_corner})
    {
        auto mesh = make_mesh();
        mesh.attributes.push_back(make_uvs(frequency));
        mesh.textures.push_back({.name = "base_color", .source = {.texture = sv::texture_id(1), .uv_attribute = "uv"}});
        mesh.textures.push_back({.name = "roughness", .source = {.texture = sv::texture_id(2), .uv_attribute = "uv"}});
        check_compiles(sv::resolve_material(materials, gold, mesh));
    }
}


TEST("sv - a generated permutation compiles as the path tracer's closest-hit")
{
    if (!sv_test::shared_env().has_compiler)
        return;

    auto materials = sv::material_library::create();
    sv::register_builtin_material_types(materials);
    auto const pbr = materials.acquire_type(sv::builtin_material::pbr).value();
    auto const gold = materials.acquire(sv::material::create("gold", pbr, {}));

    // A mesh exercising both the interpolated and the sampled path, so the hit shader is the real shape rather than a trivial one.
    auto mesh = make_mesh();
    mesh.attributes.push_back(make_uvs());
    mesh.attributes.push_back(
        sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_vertex, cc::array<f32>{0.1f, 0.2f, 0.3f}));
    mesh.textures.push_back({.name = "base_color", .source = {.texture = sv::texture_id(1), .uv_attribute = "uv"}});

    auto const resolved = sv::resolve_material(materials, gold, mesh);
    auto const g = sv::generate_material_shader(resolved, {.epilogue_include = "pt_material_hit.hlsli"});

    // The epilogue lands after the function it calls, which is the whole reason it is an epilogue.
    CHECK(g.source.find("sv_surface sv_evaluate_material") < g.source.find("#include \"pt_material_hit.hlsli\""));

    auto const& lib = *sv_test::shared_env().lib;
    auto const shader
        = lib.compile_source(g.source, sg::shader_stage::closest_hit, "PtClosestHit", sg::shader_format::dxil,
                             {.include_dir = "sv_shaders", .label = "<generated closest-hit>"});

    REQUIRE(shader != nullptr);
    (void)cc::try_async_blocking_get(shader);
    if (shader->has_error())
        FAIL(cc::format("{}\n--- source ---\n{}", shader->try_error()->underlying().to_string(), g.source));
    REQUIRE(shader->has_value());

    auto const& compiled = shader->try_value();
    CHECK(compiled->stage == sg::shader_stage::closest_hit);
    CHECK(compiled->bytecode.size() > 0);

    // The instance table and the bindless buffers must both reflect: they are what the hit reads everything else through.
    auto saw_instances = false;
    auto saw_buffers = false;
    for (auto const& b : compiled->bindings)
    {
        if (b.name == "Instances")
            saw_instances = true;
        if (b.name == sv::name_of(sv::bindless_table::buffers))
        {
            saw_buffers = true;
            // Reflection must agree with the layout the manager declares, or the two groups cannot be bound together.
            CHECK(b.space.value() == sv::space_of(sv::bindless_table::buffers));
            CHECK(b.is_array());
        }
    }
    CHECK(saw_instances);
    CHECK(saw_buffers);
}

#endif // SLIB_HAS_DXC
