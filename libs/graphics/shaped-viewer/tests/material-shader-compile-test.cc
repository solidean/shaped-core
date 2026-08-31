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
/// A compute entry that reaches the generated function the way a closest-hit will: through an `sv::instance` read out of the
/// instance table, so `sv::make_context` and the byte layout it walks are compiled too rather than only the material itself.
constexpr cc::string_view test_entry = R"hlsl(
StructuredBuffer<sv::instance> sv_test_instances : register(t0, space0);
RWStructuredBuffer<float4> sv_test_out : register(u0);

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    sv::instance inst = sv_test_instances[tid.x];
    sv::shading_context ctx = sv::make_context(inst, gBindlessBuffers[NonUniformResourceIndex(inst.indices)], 0, float2(0.25, 0.25));
    sv::surface s = sv_evaluate_material(ctx);
    sv_test_out[0] = float4(s.base_color, s.specular_roughness) + float4(s.emission_color, s.base_metalness) + float4(s.geometry_normal, s.geometry_opacity);
}
)hlsl";

/// A CPU attribute as the binding a GPU mesh carries.
/// The id is arbitrary: resolution matches on name, format and frequency, and never reaches for the buffer behind one.
[[nodiscard]] sv::mesh_attribute_binding bind(sv::mesh_attribute const& a)
{
    auto const per_instance = a.frequency == sv::attribute_frequency::per_instance;
    return sv::mesh_attribute_binding::of(a, per_instance ? sv::attribute_id::invalid : sv::attribute_id(0));
}

[[nodiscard]] sv::mesh make_mesh()
{
    // Resolution reads the lists and the summary, never the geometry itself, so a stand-in id is all this needs.
    return {.name = "tri", .geometry = sv::mesh_id(0), .triangle_count = 1, .vertex_count = 3};
}

[[nodiscard]] sv::mesh_attribute_binding make_uvs(sv::attribute_frequency f = sv::attribute_frequency::per_vertex)
{
    auto const uvs = cc::array<tg::vec2f>{tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1)};
    return bind(sv::mesh_attribute::create("uv", f, uvs));
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

    for (auto const& name : {sv::builtin_material::openpbr, sv::builtin_material::pbr, sv::builtin_material::unlit})
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
    mesh.attributes.push_back(bind(sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_vertex, colors)));
    mesh.attributes.push_back(
        bind(sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_triangle, cc::array<f32>{0.4f})));
    mesh.attributes.push_back(bind(sv::mesh_attribute::create("metallic", sv::attribute_frequency::per_corner, scalars)));
    mesh.attributes.push_back(bind(sv::mesh_attribute::create_value("occlusion", 0.5f)));
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


TEST("sv - a swizzled sample and an alpha cutout compile")
{
    if (!sv_test::shared_env().has_compiler)
        return;

    auto materials = sv::material_library::create();
    sv::register_builtin_material_types(materials);
    auto const type = materials.acquire_type(sv::builtin_material::openpbr).value();
    auto const id = materials.acquire(sv::material::create("cutout", type, {}));

    // One packed texture bound three times, the way a glTF import binds a metallic-roughness map and a base color map:
    // a letter swizzle, a narrowing to one channel, and a constant selector the letter form cannot spell.
    auto mesh = make_mesh();
    mesh.attributes.push_back(make_uvs());
    mesh.textures.push_back({.name = "base_metalness",
                             .source = {.texture = sv::texture_id(1),
                                        .uv_attribute = "uv",
                                        .swizzle = sv::channel_swizzle::of_channel(sv::texture_channel::b)}});
    mesh.textures.push_back({.name = "specular_roughness",
                             .source = {.texture = sv::texture_id(1),
                                        .uv_attribute = "uv",
                                        .swizzle = sv::channel_swizzle::of_channel(sv::texture_channel::g)}});
    mesh.textures.push_back({.name = "base_color",
                             .source = {.texture = sv::texture_id(2),
                                        .uv_attribute = "uv",
                                        .swizzle = sv::channel_swizzle::of(
                                            sv::texture_channel::r, sv::texture_channel::g, sv::texture_channel::one)}});
    // The alpha of that same base color map, which is what makes a cutout one upload rather than two.
    mesh.textures.push_back({.name = "opacity",
                             .source = {.texture = sv::texture_id(2),
                                        .uv_attribute = "uv",
                                        .swizzle = sv::channel_swizzle::of_channel(sv::texture_channel::a)}});
    // And a normal map, whose [0,1] to [-1,1] decode is a sample transform and nothing else.
    mesh.textures.push_back({.name = "normal",
                             .source = {.texture = sv::texture_id(3),
                                        .uv_attribute = "uv",
                                        .transform = sv::sample_transform::of_signed_normal(0.8f)}});

    auto const resolved = sv::resolve_material(materials, id, mesh);
    auto const g = sv::generate_material_shader(resolved);

    // Two textures sampled the same way share one sampler, whatever their swizzles say.
    CHECK(g.source.contains("SamplerState sv_sampler_0"));
    CHECK(!g.source.contains("SamplerState sv_sampler_1"));

    // A sampled opacity is what makes the permutation able to reject an intersection at all.
    CHECK(g.can_cut_out);

    check_compiles(resolved);
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
    mesh.attributes.push_back(bind(sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_vertex,
                                                              cc::array<f32>{0.1f, 0.2f, 0.3f})));
    mesh.textures.push_back({.name = "base_color", .source = {.texture = sv::texture_id(1), .uv_attribute = "uv"}});

    auto const resolved = sv::resolve_material(materials, gold, mesh);
    auto const g = sv::generate_material_shader(resolved, {.epilogue_include = "pt_material_hit.hlsli"});

    // The epilogue lands after the function it calls, which is the whole reason it is an epilogue.
    CHECK(g.source.find("sv::surface sv_evaluate_material") < g.source.find("#include \"pt_material_hit.hlsli\""));

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

/// Compiles one resolved material as the path tracer's real closest-hit, epilogue included.
void check_hit_compiles(sv::resolved_material const& r, cc::string_view label)
{
    auto const& lib = *sv_test::shared_env().lib;

    auto const g = sv::generate_material_shader(r, {.epilogue_include = "pt_material_hit.hlsli"});
    auto const shader = lib.compile_source(g.source, sg::shader_stage::closest_hit, "PtClosestHit",
                                           sg::shader_format::dxil, {.include_dir = "sv_shaders", .label = label});

    REQUIRE(shader != nullptr);
    (void)cc::try_async_blocking_get(shader);
    if (shader->has_error())
        FAIL(cc::format("{}\n--- source ---\n{}", shader->try_error()->underlying().to_string(), g.source));
    REQUIRE(shader->has_value());
    CHECK(shader->try_value()->bytecode.size() > 0);
}

TEST("sv - the openpbr closest-hit compiles with a supplied tangent frame")
{
    if (!sv_test::shared_env().has_compiler)
        return; // no DXC installed: nothing here can compile

    // The epilogue's frame path is behind `#if SV_ATTR_SUPPLIED_tangent_frame`, so a mesh carrying no frame never compiles
    // it — which is exactly how a broken quaternion path would sit unnoticed behind a green suite.
    // This is the permutation that does compile it: the rotation interpolation, the object-to-world basis and the two-sided
    // flip all land in the source only here.
    auto materials = sv::material_library::create();
    sv::register_builtin_material_types(materials);

    auto const type = materials.acquire_type(sv::builtin_material::openpbr).value();
    auto const id = materials.acquire(sv::material::create("framed", type, {}));

    auto mesh = make_mesh();
    auto const frames = cc::array<tg::vec4f>{tg::vec4f(0, 0, 0, 1), tg::vec4f(0, 0, 0, 1), tg::vec4f(0, 0, 0, 1)};
    mesh.attributes.push_back(
        bind(sv::mesh_attribute::create("tangent_frame", sv::attribute_frequency::per_vertex, frames)));

    auto const resolved = sv::resolve_material(materials, id, mesh);
    CHECK(sv::generate_material_shader(resolved).source.contains("SV_ATTR_SUPPLIED_tangent_frame 1"));

    check_hit_compiles(resolved, "<generated framed closest-hit>");
}

TEST("sv - the openpbr closest-hit compiles with the full layered BSDF")
{
    if (!sv_test::shared_env().has_compiler)
        return; // no DXC installed: nothing here can compile

    // The `openpbr` type is what puts every lobe in the source at once: the fragment writes all twenty parameters, so nothing
    // in shaders/openpbr.hlsli is dead-stripped before the compiler has seen it.
    // Compiling it as the real closest-hit is what covers the estimators and the sampling too, which the compute entry above
    // never reaches.
    auto materials = sv::material_library::create();
    sv::register_builtin_material_types(materials);

    auto const type = materials.acquire_type(sv::builtin_material::openpbr).value();
    auto const id = materials.acquire(sv::material::create("openpbr", type, {}));

    auto const resolved = sv::resolve_material(materials, id, make_mesh());

    check_hit_compiles(resolved, "<generated openpbr closest-hit>");
}

#endif // SLIB_HAS_DXC
