#include <clean-core/container/array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/test.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/material/resolve.hh>
#include <shaped-viewer/material/shader_generator.hh>
#include <shaped-viewer/scene/quadric_set.hh>
#include <shaped-viewer/scene/resident_mesh.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

// How a material reaches a surface with no vertices.
//
// There is ONE frequency set and a geometry admits the subset its own primitives number, which is what lets one material
// definition generate one shader body for both — see libs/graphics/shaped-viewer/docs/quadrics.md.
// What is pinned here is which rank won and what the generator emitted for it; that the source then COMPILES is
// quadric-trace-test's job, and only a compile says the emitted text means anything.
//
// No device: resolution and generation are both pure functions of a type, a material and a geometry view.

namespace
{
/// A material type declaring one color, which is what everything below resolves.
[[nodiscard]] sv::material_type color_type()
{
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("color", tg::vec3f(0.5f, 0.5f, 0.5f)));
    return sv::material_type::create("sv_test_color", cc::move(signature), "    surface.base_color = color;");
}

[[nodiscard]] sv::mesh_attribute_binding bind(sv::mesh_attribute const& a)
{
    auto const per_instance = a.frequency == sv::attribute_frequency::per_instance;
    return sv::mesh_attribute_binding::of(a, per_instance ? sv::attribute_id::invalid : sv::attribute_id(0));
}

/// A quadric batch carrying `a`, as the renderer's id-only form.
[[nodiscard]] sv::resident_quadric_set quadric_set_with(sv::mesh_attribute const& a)
{
    auto set = sv::resident_quadric_set{.name = "edges", .geometry = sv::quadric_set_id(0), .primitive_count = 2};
    set.attributes.push_back(bind(a));
    return set;
}

[[nodiscard]] sv::resident_mesh mesh_with(sv::mesh_attribute const& a)
{
    auto mesh = sv::resident_mesh{.name = "tri", .geometry = sv::mesh_id(0), .triangle_count = 1, .vertex_count = 3};
    mesh.attributes.push_back(bind(a));
    return mesh;
}

[[nodiscard]] bool contains(cc::string_view haystack, cc::string_view needle)
{
    return haystack.contains(needle);
}
} // namespace

TEST("sv::serves splits the geometric frequencies by what each geometry numbers")
{
    using f = sv::attribute_frequency;

    // One value for the whole placement, which any geometry has.
    CHECK(sv::serves(sv::geometry_kind::triangles, f::per_instance));
    CHECK(sv::serves(sv::geometry_kind::quadrics, f::per_instance));

    // One value per element of the primitive stream, which BOTH geometries number — and the reason the frequencies are
    // one set rather than one per kind.
    CHECK(sv::serves(sv::geometry_kind::triangles, f::per_triangle));
    CHECK(sv::serves(sv::geometry_kind::quadrics, f::per_triangle));

    // Corners and vertices are the mesh's alone; a quadric has neither.
    CHECK(sv::serves(sv::geometry_kind::triangles, f::per_vertex));
    CHECK(sv::serves(sv::geometry_kind::triangles, f::per_corner));
    CHECK(!sv::serves(sv::geometry_kind::quadrics, f::per_vertex));
    CHECK(!sv::serves(sv::geometry_kind::quadrics, f::per_corner));
    CHECK(!sv::serves(sv::geometry_kind::quadrics, f::per_edge));
}

TEST("sv::resolve_material takes a per_triangle attribute on a quadric batch")
{
    auto const type = color_type();
    auto const material = sv::material::create("m", sv::material_type_id::invalid, {});

    auto const colors = cc::array<tg::vec3f>::create_filled(2, tg::vec3f(1, 0, 0));
    auto const attribute = sv::mesh_attribute::create("color", sv::attribute_frequency::per_triangle, colors);

    // Held in a named local rather than passed as a temporary: a `resolved_material` BORROWS its geometry, so a set that
    // died at the end of the call expression would leave every attribute pointer dangling.
    auto const set = quadric_set_with(attribute);
    auto const r = sv::resolve_material(type, material, set);

    REQUIRE(r.attributes.size() == 1);
    CHECK(r.attributes[0].frequency == sv::material_frequency::mesh_attribute);
    REQUIRE(r.attributes[0].attribute != nullptr);
    CHECK(r.attributes[0].attribute->frequency == sv::attribute_frequency::per_triangle);
}

TEST("sv::resolve_material falls back when the geometry cannot number the frequency")
{
    auto const type = color_type();
    auto const material = sv::material::create("m", sv::material_type_id::invalid, {});

    // Authored for a mesh and placed on a batch: the candidate is unusable, so it loses to the coarser rank rather than
    // failing the resolve — the same thing a format mismatch does.
    auto const per_vertex = cc::array<tg::vec3f>::create_filled(3, tg::vec3f(1, 0, 0));
    auto const vertex_color = sv::mesh_attribute::create("color", sv::attribute_frequency::per_vertex, per_vertex);

    auto const batch = quadric_set_with(vertex_color);
    nx::expect_warning("attribute 'color' is bound at a frequency quadric geometry cannot number", nx::exactly(1, "s"
                                                                                                                  "v"));
    auto const on_quadrics = sv::resolve_material(type, material, batch);
    REQUIRE(on_quadrics.attributes.size() == 1);
    CHECK(on_quadrics.attributes[0].frequency == sv::material_frequency::material_type);
    CHECK(on_quadrics.attributes[0].attribute == nullptr);

    // The same attribute on a mesh IS usable, which is what says the refusal is the geometry's rather than the attribute
    // being malformed.
    auto const mesh = mesh_with(vertex_color);
    auto const on_mesh = sv::resolve_material(type, material, mesh);
    REQUIRE(on_mesh.attributes.size() == 1);
    CHECK(on_mesh.attributes[0].frequency == sv::material_frequency::mesh_attribute);
}

TEST("sv::resolve_material leaves the texture ranks unreachable on a quadric")
{
    auto const type = color_type();

    // A material naming a texture for `color`, sampled through a uv set.
    auto const material = sv::material::create(
        "m", sv::material_type_id::invalid,
        {sv::material_attribute_binding::of_texture("color", {.texture = sv::texture_id(0), .uv_attribute = "uv"})});

    auto const uvs = cc::array<tg::vec2f>::create_filled(2, tg::vec2f(0.5f, 0.5f));

    // Even carrying a uv set at a frequency it CAN number, a quadric resolves no sample: there is no surface
    // parameterization for one, so the uv lookup refuses regardless of what the batch offers.
    auto const uv = sv::mesh_attribute::create("uv", sv::attribute_frequency::per_triangle, uvs);
    auto const batch = quadric_set_with(uv);
    auto const on_quadrics = sv::resolve_material(type, material, batch);

    REQUIRE(on_quadrics.attributes.size() == 1);
    CHECK(on_quadrics.attributes[0].sample == nullptr);
    CHECK(on_quadrics.attributes[0].frequency == sv::material_frequency::material_type);

    // The same material on a mesh carrying the same uv name DOES sample, which is what says the refusal is the geometry's
    // rather than the material being malformed.
    auto const mesh_uvs = cc::array<tg::vec2f>::create_filled(3, tg::vec2f(0.5f, 0.5f));
    auto const mesh_uv = sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex, mesh_uvs);
    auto const mesh = mesh_with(mesh_uv);
    auto const on_mesh = sv::resolve_material(type, material, mesh);

    REQUIRE(on_mesh.attributes.size() == 1);
    CHECK(on_mesh.attributes[0].sample != nullptr);
    CHECK(on_mesh.attributes[0].frequency == sv::material_frequency::material_texture);
}

TEST("sv::generate_material_shader emits a flat load for per_triangle")
{
    auto const type = color_type();
    auto const material = sv::material::create("m", sv::material_type_id::invalid, {});

    auto const colors = cc::array<tg::vec3f>::create_filled(2, tg::vec3f(1, 0, 0));
    auto const attribute = sv::mesh_attribute::create("color", sv::attribute_frequency::per_triangle, colors);
    auto const set = quadric_set_with(attribute);
    auto const r = sv::resolve_material(type, material, set);

    auto const generated = sv::generate_material_shader(r);

    // One element at PrimitiveIndex(), so it loads rather than blending.
    CHECK(contains(generated.source, "sv::load_element_f3"));
    CHECK(contains(generated.source, "ctx.primitive"));
    CHECK(!contains(generated.source, "ctx.barycentrics"));
}

TEST("sv::generate_material_shader emits ONE body for a mesh and a quadric alike")
{
    // The central claim of the shared frequency set, and the thing that would silently stop being true.
    //
    // A `per_triangle` attribute resolves the same way on either geometry, so the two produce the same permutation key and
    // the same source — byte for byte.
    // Only the PREAMBLE differs, and that is the runtime include rather than anything the generator emits here.
    auto const type = color_type();
    auto const material = sv::material::create("m", sv::material_type_id::invalid, {});

    auto const colors = cc::array<tg::vec3f>::create_filled(2, tg::vec3f(1, 0, 0));
    auto const attribute = sv::mesh_attribute::create("color", sv::attribute_frequency::per_triangle, colors);

    auto const set = quadric_set_with(attribute);
    auto const mesh = mesh_with(attribute);

    auto const on_quadrics = sv::resolve_material(type, material, set);
    auto const on_mesh = sv::resolve_material(type, material, mesh);

    CHECK(on_quadrics.permutation_key == on_mesh.permutation_key);
    CHECK(sv::generate_material_shader(on_quadrics).source == sv::generate_material_shader(on_mesh).source);
}

TEST("sv::generate_material_shader forks a permutation on the geometric frequency")
{
    // What DOES fork one: the same name at a different frequency, since that picks a different load.
    auto const type = color_type();
    auto const material = sv::material::create("m", sv::material_type_id::invalid, {});

    auto const one = cc::array<tg::vec3f>::create_filled(1, tg::vec3f(1, 0, 0));
    auto const three = cc::array<tg::vec3f>::create_filled(3, tg::vec3f(1, 0, 0));

    auto const flat_mesh = mesh_with(sv::mesh_attribute::create("color", sv::attribute_frequency::per_triangle, three));
    auto const blended_mesh = mesh_with(sv::mesh_attribute::create("color", sv::attribute_frequency::per_vertex, three));
    auto const constant_mesh = mesh_with(sv::mesh_attribute::create("color", sv::attribute_frequency::per_instance, one));

    auto const flat = sv::resolve_material(type, material, flat_mesh);
    auto const blended = sv::resolve_material(type, material, blended_mesh);
    auto const constant = sv::resolve_material(type, material, constant_mesh);

    CHECK(flat.permutation_key != blended.permutation_key);

    // And a per_instance value is a constant rather than an indexed load, so it forks again.
    CHECK(constant.permutation_key != flat.permutation_key);
}
