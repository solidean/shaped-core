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

// How a material reaches a surface with no vertices: the quadric frequencies, and what resolution does with a frequency the
// geometry cannot number.
//
// No device — resolution and generation are both pure functions of a type, a material and a geometry view.
// That the generated source then COMPILES is material-shader-compile-test's job; what is pinned here is which rank won and what
// the generator emitted for it.

namespace
{
/// A material type declaring one colour, which is what everything below resolves.
[[nodiscard]] sv::material_type colour_type()
{
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("colour", tg::vec3f(0.5f, 0.5f, 0.5f)));
    return sv::material_type::create("sv_test_colour", cc::move(signature), "    surface.base_color = colour;");
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

TEST("sv::serves splits the geometric frequencies by geometry kind")
{
    using f = sv::attribute_frequency;

    // One value for the whole placement, which any geometry has.
    CHECK(sv::serves(sv::geometry_kind::triangles, f::per_instance));
    CHECK(sv::serves(sv::geometry_kind::quadrics, f::per_instance));

    // Everything else indexes something only one kind of geometry numbers.
    CHECK(sv::serves(sv::geometry_kind::triangles, f::per_vertex));
    CHECK(sv::serves(sv::geometry_kind::triangles, f::per_corner));
    CHECK(sv::serves(sv::geometry_kind::triangles, f::per_triangle));
    CHECK(!sv::serves(sv::geometry_kind::triangles, f::per_triangle));
    CHECK(!sv::serves(sv::geometry_kind::triangles, f::per_triangle));

    CHECK(sv::serves(sv::geometry_kind::quadrics, f::per_triangle));
    CHECK(sv::serves(sv::geometry_kind::quadrics, f::per_triangle));
    CHECK(!sv::serves(sv::geometry_kind::quadrics, f::per_vertex));
    CHECK(!sv::serves(sv::geometry_kind::quadrics, f::per_corner));
    CHECK(!sv::serves(sv::geometry_kind::quadrics, f::per_triangle));
}

TEST("sv::resolve_material takes a per_triangle attribute on a quadric batch")
{
    auto const type = colour_type();
    auto const material = sv::material::create("m", sv::material_type_id::invalid, {});

    auto const colours = cc::array<tg::vec3f>::create_filled(2, tg::vec3f(1, 0, 0));
    auto const attribute = sv::mesh_attribute::create("colour", sv::attribute_frequency::per_triangle, colours);

    auto const r = sv::resolve_material(type, material, quadric_set_with(attribute));

    REQUIRE(r.attributes.size() == 1);
    CHECK(r.attributes[0].frequency == sv::material_frequency::mesh_attribute);
    REQUIRE(r.attributes[0].attribute != nullptr);
    CHECK(r.attributes[0].attribute->frequency == sv::attribute_frequency::per_triangle);
}

TEST("sv::resolve_material falls back when the geometry cannot number the frequency")
{
    auto const type = colour_type();
    auto const material = sv::material::create("m", sv::material_type_id::invalid, {});

    // Authored for a mesh and placed on a batch: the candidate is unusable, so it loses to the coarser rank rather than
    // failing the resolve — the same thing a format mismatch does.
    auto const per_vertex = cc::array<tg::vec3f>::create_filled(3, tg::vec3f(1, 0, 0));
    auto const vertex_colour = sv::mesh_attribute::create("colour", sv::attribute_frequency::per_vertex, per_vertex);

    auto const on_quadrics = sv::resolve_material(type, material, quadric_set_with(vertex_colour));
    REQUIRE(on_quadrics.attributes.size() == 1);
    CHECK(on_quadrics.attributes[0].frequency == sv::material_frequency::material_type);
    CHECK(on_quadrics.attributes[0].attribute == nullptr);

    // And the other way round, which is what keeps one attribute list from being fatal on either geometry.
    auto const per_triangle = cc::array<tg::vec3f>::create_filled(2, tg::vec3f(0, 1, 0));
    auto const quadric_colour = sv::mesh_attribute::create("colour", sv::attribute_frequency::per_triangle, per_triangle);

    auto const on_mesh = sv::resolve_material(type, material, mesh_with(quadric_colour));
    REQUIRE(on_mesh.attributes.size() == 1);
    CHECK(on_mesh.attributes[0].frequency == sv::material_frequency::material_type);
}

TEST("sv::resolve_material leaves the texture ranks unreachable on a quadric")
{
    auto const type = colour_type();

    // A material naming a texture for `colour`, sampled through a uv set.
    auto const material = sv::material::create(
        "m", sv::material_type_id::invalid,
        {sv::material_attribute_binding::of_texture("colour", {.texture = sv::texture_id(0), .uv_attribute = "uv"})});

    auto const uvs = cc::array<tg::vec2f>::create_filled(2, tg::vec2f(0.5f, 0.5f));

    // Even carrying a uv set at a frequency it CAN number, a quadric resolves no sample: there is no surface
    // parametrization for one, so the uv lookup refuses regardless of what the batch offers.
    auto const uv = sv::mesh_attribute::create("uv", sv::attribute_frequency::per_triangle, uvs);
    auto const on_quadrics = sv::resolve_material(type, material, quadric_set_with(uv));

    REQUIRE(on_quadrics.attributes.size() == 1);
    CHECK(on_quadrics.attributes[0].sample == nullptr);
    CHECK(on_quadrics.attributes[0].frequency == sv::material_frequency::material_type);

    // The same material on a mesh carrying the same uv name DOES sample, which is what says the refusal is the geometry's
    // rather than the material being malformed.
    auto const mesh_uvs = cc::array<tg::vec2f>::create_filled(3, tg::vec2f(0.5f, 0.5f));
    auto const mesh_uv = sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex, mesh_uvs);
    auto const on_mesh = sv::resolve_material(type, material, mesh_with(mesh_uv));

    REQUIRE(on_mesh.attributes.size() == 1);
    CHECK(on_mesh.attributes[0].sample != nullptr);
    CHECK(on_mesh.attributes[0].frequency == sv::material_frequency::material_texture);
}

TEST("sv::generate_material_shader emits a flat load for per_triangle")
{
    auto const type = colour_type();
    auto const material = sv::material::create("m", sv::material_type_id::invalid, {});

    auto const colours = cc::array<tg::vec3f>::create_filled(2, tg::vec3f(1, 0, 0));
    auto const attribute = sv::mesh_attribute::create("colour", sv::attribute_frequency::per_triangle, colours);
    auto const r = sv::resolve_material(type, material, quadric_set_with(attribute));

    auto const generated = sv::generate_material_shader(r);

    // `per_triangle` means exactly what `per_triangle` does — one element at PrimitiveIndex() — so it loads rather than blends.
    CHECK(contains(generated.source, "sv::load_element_f3"));
    CHECK(contains(generated.source, "ctx.primitive"));
    CHECK(!contains(generated.source, "ctx.barycentrics"));
}

TEST("sv::generate_material_shader blends the ends for per_triangle")
{
    auto const type = colour_type();
    auto const material = sv::material::create("m", sv::material_type_id::invalid, {});

    // Two values per primitive, which is what an edge fading along its length is authored as.
    auto const colours = cc::array<tg::vec3f>::create_filled(4, tg::vec3f(1, 0, 0));
    auto const attribute = sv::mesh_attribute::create("colour", sv::attribute_frequency::per_triangle, colours);

    auto const set = quadric_set_with(attribute);
    auto const r = sv::resolve_material(type, material, set);

    REQUIRE(r.attributes[0].attribute != nullptr);
    CHECK(r.attributes[0].attribute->frequency == sv::attribute_frequency::per_triangle);

    auto const generated = sv::generate_material_shader(r);

    CHECK(contains(generated.source, "sv::interpolate_ends_f3"));
    CHECK(contains(generated.source, "ctx.end_blend"));
    CHECK(!contains(generated.source, "ctx.barycentrics"));
}

TEST("sv::generate_material_shader forks a permutation on the geometry's frequency")
{
    auto const type = colour_type();
    auto const material = sv::material::create("m", sv::material_type_id::invalid, {});

    auto const flat = cc::array<tg::vec3f>::create_filled(2, tg::vec3f(1, 0, 0));
    auto const ends = cc::array<tg::vec3f>::create_filled(4, tg::vec3f(1, 0, 0));

    auto const a = sv::resolve_material(
        type, material,
        quadric_set_with(sv::mesh_attribute::create("colour", sv::attribute_frequency::per_triangle, flat)));
    auto const b = sv::resolve_material(
        type, material,
        quadric_set_with(sv::mesh_attribute::create("colour", sv::attribute_frequency::per_triangle, ends)));

    // The geometric frequency picks the load code, so it is SHAPE: the two cannot share a generated shader.
    CHECK(a.permutation_key != b.permutation_key);
}
