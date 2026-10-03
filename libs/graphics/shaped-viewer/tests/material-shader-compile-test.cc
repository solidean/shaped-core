#include <shaped-shader-library/compiler/dxc_compiler.hh>

// The SGL reaches DXIL through DXC, whose reflection reads a container beside the bytecode through the Windows SDK's
// d3d12shader.h — a header the Linux DXC release does not ship.
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
#include <shaped-viewer/scene/resident_mesh.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

// The generated hit groups, put through a real compiler.
//
// material-shader-generator-test checks the TEXT; this checks that the text is valid SGL, and that what SGL emits DXC accepts.
// Every entry point a group declares is compiled on its own: the closest hit, the cutout any hits, the quadric intersection.
// The tracer compiles them through `slib::compile_hit_group`, which takes a context; this needs none.

namespace
{
/// A CPU attribute as the binding a GPU mesh carries.
/// The id is arbitrary: resolution matches on name, format and frequency, and never reaches for the buffer behind one.
[[nodiscard]] sv::mesh_attribute_binding bind(sv::mesh_attribute const& a)
{
    auto const per_instance = a.frequency == sv::attribute_frequency::per_instance;
    return sv::mesh_attribute_binding::of(a, per_instance ? sv::attribute_id::invalid : sv::attribute_id(0));
}

[[nodiscard]] sv::resident_mesh make_mesh()
{
    // Resolution reads the lists and the summary, never the geometry itself, so a stand-in id is all this needs.
    return {.name = "tri", .geometry = sv::mesh_id(0), .triangle_count = 1, .vertex_count = 3};
}

[[nodiscard]] sv::mesh_attribute_binding make_uvs(sv::attribute_frequency f = sv::attribute_frequency::per_vertex)
{
    auto const uvs = cc::array<tg::vec2f>{tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1)};
    return bind(sv::mesh_attribute::create("uv", f, uvs));
}

/// One entry point of `g`'s hit group, compiled; fails the test with the compiler's message and the source.
void check_entry_compiles(sv::generated_material_shader const& g, sg::shader_stage stage, cc::string_view entry)
{
    auto const& lib = *sv_test::shared_env().lib;
    auto const shader = lib.compile_source(g.source, stage, entry, sg::shader_format::dxil,
                                           {.language = slib::shader_language::sgl, .label = "<generated hit group>"});
    REQUIRE(shader != nullptr);
    (void)cc::try_async_blocking_get(shader);
    if (shader->has_error())
        FAIL(cc::format("{}: {}\n--- source ---\n{}", entry, shader->try_error()->underlying().to_string(), g.source));
    REQUIRE(shader->has_value());
    CHECK(shader->try_value()->bytecode.size() > 0);
}

/// Every entry point the hit group of `r` declares, for geometry of `kind`.
void check_compiles(sv::resolved_material const& r, sv::geometry_kind kind = sv::geometry_kind::triangles)
{
    auto const g = sv::generate_material_shader(r, {.kind = kind});
    REQUIRE(!g.source.empty());
    CHECK(g.source.contains("hit_group sv_material for path_rays"));
    check_entry_compiles(g, sg::shader_stage::closest_hit, "sv_closest_hit");
    if (kind == sv::geometry_kind::quadrics)
        check_entry_compiles(g, sg::shader_stage::intersection, "sv_intersection");
    else if (g.can_cut_out)
    {
        check_entry_compiles(g, sg::shader_stage::any_hit, "sv_any_hit");
        check_entry_compiles(g, sg::shader_stage::any_hit, "sv_shadow_any_hit");
    }
}
} // namespace

TEST("sv - every builtin material type compiles as a hit group, on triangles and on quadrics")
{
    if (!sv_test::shared_env().has_compiler)
        return;

    auto materials = sv::material_library::create();
    sv::register_builtin_material_types(materials);

    for (auto const& name : {sv::builtin_material::openpbr, sv::builtin_material::pbr, sv::builtin_material::unlit})
    {
        auto const type = materials.acquire_type(name).value();
        auto const id = materials.acquire(sv::material::create(cc::string(name), type, {}));
        check_compiles(sv::resolve_material(materials, id, make_mesh()));
        auto const on_quadrics = sv::resolve_material(materials.get_type(type), materials.get(id),
                                                      sv::geometry_view{.kind = sv::geometry_kind::quadrics});
        check_compiles(on_quadrics, sv::geometry_kind::quadrics);
    }
}

TEST("sv - a hit group compiles at every attribute frequency, sampled, swizzled and cut out")
{
    if (!sv_test::shared_env().has_compiler)
        return;

    auto materials = sv::material_library::create();
    sv::register_builtin_material_types(materials);
    auto const type = materials.acquire_type(sv::builtin_material::openpbr).value();
    auto const id = materials.acquire(sv::material::create("everything", type, {}));

    auto const scalars = cc::array<f32>{0.1f, 0.2f, 0.3f};
    auto const colors = cc::array<tg::vec3f>{tg::vec3f(1, 0, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, 1)};
    auto const frames = cc::array<tg::vec4f>{tg::vec4f(0, 0, 0, 1), tg::vec4f(0, 0, 0, 1), tg::vec4f(0, 0, 0, 1)};

    // Every load shape at once: interpolated, flat, per corner, per instance, a rotation, and samples a letter swizzle,
    // a narrowing, a constant selector and a transform each spell differently.
    auto mesh = make_mesh();
    mesh.attributes.push_back(make_uvs(sv::attribute_frequency::per_corner));
    mesh.attributes.push_back(
        bind(sv::mesh_attribute::create("subsurface_color", sv::attribute_frequency::per_vertex, colors)));
    mesh.attributes.push_back(
        bind(sv::mesh_attribute::create("coat_weight", sv::attribute_frequency::per_triangle, cc::array<f32>{0.4f})));
    mesh.attributes.push_back(
        bind(sv::mesh_attribute::create("fuzz_weight", sv::attribute_frequency::per_corner, scalars)));
    mesh.attributes.push_back(bind(sv::mesh_attribute::create_value("thin_film_weight", 0.5f)));
    mesh.attributes.push_back(
        bind(sv::mesh_attribute::create("tangent_frame", sv::attribute_frequency::per_vertex, frames)));
    mesh.textures.push_back({.name = "base_metalness",
                             .source = {.texture = sv::texture_id(1),
                                        .uv_attribute = "uv",
                                        .swizzle = sv::channel_swizzle::of_channel(sv::texture_channel::b)}});
    mesh.textures.push_back({.name = "base_color",
                             .source = {.texture = sv::texture_id(2),
                                        .uv_attribute = "uv",
                                        .sampler = {.mag_filter = sg::sampler_filter::nearest,
                                                    .address_u = sg::sampler_address_mode::clamp_edge},
                                        .swizzle = sv::channel_swizzle::of(
                                            sv::texture_channel::r, sv::texture_channel::g, sv::texture_channel::one)}});
    mesh.textures.push_back({.name = "opacity",
                             .source = {.texture = sv::texture_id(2),
                                        .uv_attribute = "uv",
                                        .swizzle = sv::channel_swizzle::of_channel(sv::texture_channel::a)}});
    mesh.textures.push_back({.name = "normal",
                             .source = {.texture = sv::texture_id(3),
                                        .uv_attribute = "uv",
                                        .transform = sv::sample_transform::of_signed_normal(0.8f)}});

    auto const resolved = sv::resolve_material(materials, id, mesh);
    auto const g = sv::generate_material_shader(resolved);
    CHECK(g.can_cut_out);
    CHECK(g.source.contains("const sv_supplied_tangent_frame = true"));
    CHECK(g.source.contains("palette_nearest_clamp_repeat"));
    CHECK(g.source.contains("palette_linear_repeat_repeat"));
    check_compiles(resolved);
}

TEST("sv - a hit group compiles at every attribute frequency of the pbr type")
{
    if (!sv_test::shared_env().has_compiler)
        return;

    auto materials = sv::material_library::create();
    sv::register_builtin_material_types(materials);
    auto const pbr = materials.acquire_type(sv::builtin_material::pbr).value();
    auto const gold = materials.acquire(sv::material::create("gold", pbr, {}));

    auto const scalars = cc::array<f32>{0.1f, 0.2f, 0.3f};
    auto const colors = cc::array<tg::vec3f>{tg::vec3f(1, 0, 0), tg::vec3f(0, 1, 0), tg::vec3f(0, 0, 1)};

    // A per-vertex vector and a per-triangle scalar together, so the interpolated and the flat load are both in one group.
    auto mesh = make_mesh();
    mesh.attributes.push_back(bind(sv::mesh_attribute::create("base_color", sv::attribute_frequency::per_vertex, colors)));
    mesh.attributes.push_back(
        bind(sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_triangle, cc::array<f32>{0.4f})));
    mesh.attributes.push_back(bind(sv::mesh_attribute::create("metallic", sv::attribute_frequency::per_corner, scalars)));
    mesh.attributes.push_back(bind(sv::mesh_attribute::create_value("occlusion", 0.5f)));
    check_compiles(sv::resolve_material(materials, gold, mesh));

    // Sampled, at either uv frequency.
    for (auto const frequency : {sv::attribute_frequency::per_vertex, sv::attribute_frequency::per_corner})
    {
        auto sampled = make_mesh();
        sampled.attributes.push_back(make_uvs(frequency));
        sampled.textures.push_back({.name = "base_color", .source = {.texture = sv::texture_id(1), .uv_attribute = "uv"}});
        sampled.textures.push_back({.name = "roughness", .source = {.texture = sv::texture_id(2), .uv_attribute = "uv"}});
        check_compiles(sv::resolve_material(materials, gold, sampled));
    }
}

#endif // SLIB_HAS_DXC
