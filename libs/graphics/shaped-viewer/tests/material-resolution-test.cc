#include <clean-core/container/array.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/resolve.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

// CPU-only tests for the material frequency chain.
// No GPU and no shader: a material type is a signature plus a string, and resolution is a pure function over it, a material and a
// mesh — which is the whole reason the chain was built before anything that draws.
//
// What these pin is the precedence itself: which rank wins, what `final` blocks, and the two-key split that decides whether two
// materials share a generated shader.

namespace
{
constexpr auto uv_format = sv::attribute_format::of_vector(sv::scalar_type::f32, 2);

/// A one-triangle mesh, which is enough for every frequency: three vertices, three corners, one triangle.
[[nodiscard]] sv::mesh make_mesh()
{
    auto const positions = cc::array<tg::pos3f>{tg::pos3f(0, 0, 0), tg::pos3f(1, 0, 0), tg::pos3f(0, 1, 0)};
    return {.name = "tri", .geometry = sv::triangle_geometry::create_from_positions(positions)};
}

/// A uv set at `name`, so a texture-sourced attribute has something to sample through.
[[nodiscard]] sv::mesh_attribute make_uvs(cc::string name)
{
    auto const uvs = cc::array<tg::vec2f>{tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1)};
    return sv::mesh_attribute::create(cc::move(name), sv::attribute_frequency::per_vertex, uvs);
}

/// A type with one f32 attribute, which is all the chain needs to be exercised.
[[nodiscard]] sv::material_type make_type(bool final_default = false)
{
    auto signature = cc::vector<sv::material_attribute_decl>();
    signature.push_back(sv::material_attribute_decl::of("roughness", 0.5f, final_default));
    return sv::material_type::create("test", cc::move(signature), "surface.roughness = roughness;");
}

[[nodiscard]] sv::texture_sample_source make_sample(sv::texture_id id, cc::string uv = "uv")
{
    return {.texture = id, .uv_attribute = cc::move(uv)};
}

/// The single resolved attribute, and the constant it carries when it has one.
[[nodiscard]] f32 constant_of(sv::resolved_material const& r)
{
    auto const values = r.attributes[0].constant.try_reinterpret_as<f32 const>();
    CHECK(values.has_value());
    CHECK(values.value().size() == 1);
    return values.value()[0];
}
} // namespace

TEST("sv::resolve_material - each frequency overrides its parent")
{
    auto const type = make_type();

    // Nothing bound anywhere: the declaration's own default is what a mesh with no data resolves to.
    {
        auto const m = sv::material::create("bare", sv::material_type_id(0), {});
        auto const r = sv::resolve_material(type, m, make_mesh());
        CHECK(r.attributes.size() == 1);
        CHECK(r.attributes[0].frequency == sv::material_frequency::material_type);
        CHECK(constant_of(r) == 0.5f);
    }

    // A constant on the material beats the type's default.
    auto overrides = cc::vector<sv::material_attribute_binding>();
    overrides.push_back(sv::material_attribute_binding::of("roughness", 0.25f));
    auto const rough = sv::material::create("rough", sv::material_type_id(0), overrides);
    {
        auto const r = sv::resolve_material(type, rough, make_mesh());
        CHECK(r.attributes[0].frequency == sv::material_frequency::material);
        CHECK(constant_of(r) == 0.25f);
    }

    // A per_instance mesh attribute beats the material.
    auto instanced = make_mesh();
    instanced.attributes.push_back(sv::mesh_attribute::create_value("roughness", 0.75f));
    {
        auto const r = sv::resolve_material(type, rough, instanced);
        CHECK(r.attributes[0].frequency == sv::material_frequency::mesh_instance);
        CHECK(constant_of(r) == 0.75f);
    }

    // A geometric mesh attribute beats the per_instance one, and carries no constant — it is an indexed load.
    auto per_corner = instanced;
    auto const corners = cc::array<f32>{0.1f, 0.2f, 0.3f};
    per_corner.attributes.push_back(sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_corner, corners));
    {
        auto const r = sv::resolve_material(type, rough, per_corner);
        CHECK(r.attributes[0].frequency == sv::material_frequency::mesh_attribute);
        CHECK(r.attributes[0].attribute != nullptr);
        CHECK(r.attributes[0].attribute->frequency == sv::attribute_frequency::per_corner);
    }

    // A texture the material names beats every mesh attribute — it is the finer variation.
    auto textured = per_corner;
    textured.attributes.push_back(make_uvs("uv"));
    auto sampled = cc::vector<sv::material_attribute_binding>();
    sampled.push_back(sv::material_attribute_binding::of_texture("roughness", make_sample(sv::texture_id(7))));
    auto const mat_textured = sv::material::create("mat_tex", sv::material_type_id(0), sampled);
    {
        auto const r = sv::resolve_material(type, mat_textured, textured);
        CHECK(r.attributes[0].frequency == sv::material_frequency::material_texture);
        CHECK(r.attributes[0].sample->texture == sv::texture_id(7));
    }

    // A texture the mesh offers beats the one the material named: both vary per pixel, and the mesh is the more specific owner.
    auto mesh_textured = textured;
    mesh_textured.textures.push_back({.name = "roughness", .source = make_sample(sv::texture_id(9))});
    {
        auto const r = sv::resolve_material(type, mat_textured, mesh_textured);
        CHECK(r.attributes[0].frequency == sv::material_frequency::mesh_texture);
        CHECK(r.attributes[0].sample->texture == sv::texture_id(9));
    }
}

TEST("sv::resolve_material - final blocks every finer frequency")
{
    // The case the feature exists for: the mesh has a roughness texture we know to be bad, and a final binding refuses it.
    auto mesh = make_mesh();
    mesh.attributes.push_back(make_uvs("uv"));
    mesh.attributes.push_back(sv::mesh_attribute::create_value("roughness", 0.75f));
    mesh.textures.push_back({.name = "roughness", .source = make_sample(sv::texture_id(9))});

    auto const type = make_type();

    auto pinned = cc::vector<sv::material_attribute_binding>();
    pinned.push_back(sv::material_attribute_binding::of("roughness", 0.25f, true));
    auto const m = sv::material::create("pinned", sv::material_type_id(0), pinned);

    auto const r = sv::resolve_material(type, m, mesh);
    CHECK(r.attributes[0].frequency == sv::material_frequency::material);
    CHECK(constant_of(r) == 0.25f);

    // The same binding without `final` loses to the mesh's texture, which is what makes the flag the load-bearing part.
    auto loose = cc::vector<sv::material_attribute_binding>();
    loose.push_back(sv::material_attribute_binding::of("roughness", 0.25f));
    auto const open = sv::material::create("open", sv::material_type_id(0), loose);
    CHECK(sv::resolve_material(type, open, mesh).attributes[0].frequency == sv::material_frequency::mesh_texture);
}

TEST("sv::resolve_material - a final declaration cannot be overridden at all")
{
    auto mesh = make_mesh();
    mesh.attributes.push_back(sv::mesh_attribute::create_value("roughness", 0.75f));

    auto const type = make_type(true);
    auto overrides = cc::vector<sv::material_attribute_binding>();
    overrides.push_back(sv::material_attribute_binding::of("roughness", 0.25f));
    auto const m = sv::material::create("m", sv::material_type_id(0), overrides);

    auto const r = sv::resolve_material(type, m, mesh);
    CHECK(r.attributes[0].frequency == sv::material_frequency::material_type);
    CHECK(constant_of(r) == 0.5f);
}

TEST("sv::resolve_material - a candidate that cannot be used falls back rather than failing")
{
    auto const type = make_type();
    auto const m = sv::material::create("m", sv::material_type_id(0), {});

    // Right name, wrong type: the mesh carries roughness as a vec3f, which is not what the signature declares.
    auto wrong_format = make_mesh();
    auto const colors = cc::array<tg::vec3f>{tg::vec3f(1, 0, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, 1)};
    wrong_format.attributes.push_back(
        sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_vertex, colors));
    CHECK(sv::resolve_material(type, m, wrong_format).attributes[0].frequency == sv::material_frequency::material_type);

    // A texture whose uv attribute the mesh does not carry cannot be sampled, so it loses its turn.
    auto no_uvs = make_mesh();
    no_uvs.textures.push_back({.name = "roughness", .source = make_sample(sv::texture_id(9))});
    CHECK(sv::resolve_material(type, m, no_uvs).attributes[0].frequency == sv::material_frequency::material_type);

    // Uvs at the wrong frequency are no uvs: one coordinate for the whole mesh samples a single texel.
    auto instance_uvs = make_mesh();
    instance_uvs.attributes.push_back(sv::mesh_attribute::create_value("uv", tg::vec2f(0.5f, 0.5f)));
    instance_uvs.textures.push_back({.name = "roughness", .source = make_sample(sv::texture_id(9))});
    CHECK(sv::resolve_material(type, m, instance_uvs).attributes[0].frequency == sv::material_frequency::material_type);
}

TEST("sv::resolve_material - the two keys separate the shader from its parameters")
{
    auto const type = make_type();
    auto const mesh = make_mesh();

    auto gold_b = cc::vector<sv::material_attribute_binding>();
    gold_b.push_back(sv::material_attribute_binding::of("roughness", 0.2f));
    auto copper_b = cc::vector<sv::material_attribute_binding>();
    copper_b.push_back(sv::material_attribute_binding::of("roughness", 0.6f));

    auto const gold = sv::resolve_material(type, sv::material::create("gold", sv::material_type_id(0), gold_b), mesh);
    auto const copper
        = sv::resolve_material(type, sv::material::create("copper", sv::material_type_id(0), copper_b), mesh);

    // Two materials differing only in their constants are ONE generated shader and two parameter slots.
    CHECK(gold.permutation_key == copper.permutation_key);
    CHECK(gold.parameter_key != copper.parameter_key);

    // Sampling a texture is the one thing that forces a second permutation.
    auto textured_mesh = make_mesh();
    textured_mesh.attributes.push_back(make_uvs("uv"));
    textured_mesh.textures.push_back({.name = "roughness", .source = make_sample(sv::texture_id(3))});
    auto const sampled
        = sv::resolve_material(type, sv::material::create("gold", sv::material_type_id(0), gold_b), textured_mesh);
    CHECK(sampled.permutation_key != gold.permutation_key);

    // Two meshes sampling different textures the same way still share one shader — the id is a parameter, not a permutation.
    auto other_texture = textured_mesh;
    other_texture.textures[0].source.texture = sv::texture_id(4);
    auto const sampled_other
        = sv::resolve_material(type, sv::material::create("gold", sv::material_type_id(0), gold_b), other_texture);
    CHECK(sampled_other.permutation_key == sampled.permutation_key);
    CHECK(sampled_other.parameter_key != sampled.parameter_key);
}

TEST("sv::material_library - content addressing, names and validation")
{
    auto lib = sv::material_library::create();
    sv::register_builtin_material_types(lib);

    auto const pbr = lib.acquire_type(sv::builtin_material::pbr);
    CHECK(pbr.has_value());
    CHECK(lib.acquire_type(sv::builtin_material::unlit).has_value());
    CHECK(!lib.acquire_type("nope").has_value());

    // Registering the same type again is the id already resident, which is what lets a caller re-register every frame.
    auto const before = lib.type_count();
    sv::register_builtin_material_types(lib);
    CHECK(lib.type_count() == before);

    CHECK(lib.get_type(pbr.value()).name == sv::builtin_material::pbr);
    CHECK(lib.get_type(pbr.value()).find("roughness") != nullptr);
    CHECK(lib.get_type(pbr.value()).find("roughnes") == nullptr);

    auto overrides = cc::vector<sv::material_attribute_binding>();
    overrides.push_back(sv::material_attribute_binding::of("roughness", 0.1f));
    auto const gold = lib.acquire(sv::material::create("gold", pbr.value(), overrides));
    CHECK(lib.acquire(sv::material::create("gold", pbr.value(), overrides)) == gold);
    CHECK(lib.material_count() == 1);
    CHECK(lib.acquire("gold").value() == gold);
    CHECK(lib.get(gold).name == "gold");

    // Resolving through the library is the same answer as resolving the pair by hand.
    auto const mesh = make_mesh();
    auto const via_lib = sv::resolve_material(lib, gold, mesh);
    auto const direct = sv::resolve_material(lib.get_type(pbr.value()), lib.get(gold), mesh);
    CHECK(via_lib.permutation_key == direct.permutation_key);
    CHECK(via_lib.parameter_key == direct.parameter_key);
}

TEST("sv::acquire_material_library - the library is created once and shared")
{
    auto builds = 0;
    static auto custom = sv::material_library::create();

    sv::set_acquire_material_library(
        [&builds]
        {
            ++builds;
            return &custom;
        });

    auto const first = sv::acquire_material_library();
    CHECK(first.has_value());
    auto const second = sv::acquire_material_library();
    CHECK(second.value() == first.value());
    CHECK(builds == 1);

    // Clearing the hook does not un-cache what it already answered with.
    sv::set_acquire_material_library({});
    CHECK(sv::acquire_material_library().value() == first.value());
}
