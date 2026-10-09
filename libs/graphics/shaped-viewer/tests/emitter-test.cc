#include <clean-core/container/array.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/resolve.hh>
#include <shaped-viewer/material/shader_generator.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/rendering/view_renderer.hh>
#include <shaped-viewer/resources/instance_data.hh>
#include <shaped-viewer/scene/resident_mesh.hh>
#include <typed-geometry/linalg/quat.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/transform/compose.hh>

using namespace cc::primitive_defines;

// CPU-only tests of emissive triangles as lights: the type contract, the rule that makes an instance a light, and its place in the light table.
// What the tracers make of them is emitter-trace-test.cc, which needs a device.

namespace
{
/// A CPU attribute as the binding a GPU mesh carries; the id is arbitrary, since resolution never reaches for the buffer.
[[nodiscard]] sv::mesh_attribute_binding bind(sv::mesh_attribute const& a)
{
    auto const per_instance = a.frequency == sv::attribute_frequency::per_instance;
    return sv::mesh_attribute_binding::of(a, per_instance ? sv::attribute_id::invalid : sv::attribute_id(0));
}

[[nodiscard]] sv::resident_mesh make_mesh()
{
    return {.name = "tri", .geometry = sv::mesh_id(0), .triangle_count = 1, .vertex_count = 3};
}

/// A library holding the builtin types, for one test.
[[nodiscard]] sv::material_library make_library()
{
    auto lib = sv::material_library::create();
    sv::register_builtin_material_types(lib);
    return lib;
}

/// The permutation of `type_name` under `overrides`, resolved against `mesh`.
[[nodiscard]] sv::generated_material_shader generate(sv::material_library& lib,
                                                     cc::string_view type_name,
                                                     cc::vector<sv::material_attribute_binding> overrides,
                                                     sv::resident_mesh const& mesh = make_mesh())
{
    auto const type = lib.acquire_type(type_name).value();
    auto const id = lib.acquire(sv::material::create("emitter", type, cc::move(overrides)));
    return sv::generate_material_shader(sv::resolve_material(lib, id, mesh));
}

/// The parameter block of `type_name` under `overrides` as the resource manager keeps it: its constant slots, by the layout.
[[nodiscard]] sv::instance_record block_of(sv::material_library& lib,
                                           cc::string_view type_name,
                                           cc::vector<sv::material_attribute_binding> overrides,
                                           sv::resident_mesh const& mesh = make_mesh())
{
    auto const type = lib.acquire_type(type_name).value();
    auto const id = lib.acquire(sv::material::create("emitter", type, cc::move(overrides)));
    auto const resolved = sv::resolve_material(lib, id, mesh);
    auto const g = sv::generate_material_shader(resolved);
    auto record = sv::instance_record{.size_bytes = g.layout.size_bytes};
    for (auto const& slot : g.layout.slots)
    {
        auto s = sv::instance_slot{.kind = slot.kind, .offset = slot.offset, .size_bytes = slot.size_bytes};
        if (slot.kind == sv::material_slot_kind::constant)
        {
            auto const constant = resolved.attributes[slot.attribute_index].constant;
            s.constant = cc::vector<byte>::create_copy_of(constant);
        }
        record.slots.push_back(cc::move(s));
    }
    return record;
}

[[nodiscard]] sv::material_emission emission_of(sv::material_library& lib,
                                                cc::string_view type_name,
                                                cc::vector<sv::material_attribute_binding> overrides,
                                                sv::resident_mesh const& mesh = make_mesh())
{
    return generate(lib, type_name, cc::move(overrides), mesh).emission;
}
} // namespace

TEST("sv - only a type that declares its emission lights the scene, and unlit stays a display material")
{
    auto lib = make_library();

    auto const& openpbr = lib.get_type(lib.acquire_type(sv::builtin_material::openpbr).value());
    CHECK(openpbr.emission_color_attribute == "emission_color");
    CHECK(openpbr.emission_scale_attribute == "emission_luminance");
    CHECK(openpbr.declares_emission());

    auto const& pbr = lib.get_type(lib.acquire_type(sv::builtin_material::pbr).value());
    CHECK(pbr.emission_color_attribute == "emissive");
    CHECK(pbr.emission_scale_attribute.empty());

    auto const& unlit = lib.get_type(lib.acquire_type(sv::builtin_material::unlit).value());
    CHECK(!unlit.declares_emission());
    CHECK(!emission_of(lib, sv::builtin_material::unlit, {}).declares());

    // A declaring type's fragment writes no emission of its own, so the light sampler and the closest hit read one value.
    CHECK(!cc::string_view(openpbr.shader).contains("surface.emission_"));
    CHECK(!cc::string_view(pbr.shader).contains("surface.emission_"));
}

TEST("sv - the generator writes a declared emission after the fragment, clamped as the light sampler reads it")
{
    auto lib = make_library();

    auto const openpbr = generate(lib, sv::builtin_material::openpbr, {});
    auto const fragment_at = cc::string_view(openpbr.source).find("// --- openpbr ---");
    auto const color_at
        = cc::string_view(openpbr.source).find("surface.emission_color = max(float3(0.0), emission_color)");
    auto const scale_at
        = cc::string_view(openpbr.source).find("surface.emission_luminance = max(0.0, emission_luminance)");
    REQUIRE(fragment_at >= 0);
    CHECK(color_at > fragment_at);
    CHECK(scale_at > fragment_at);
    // The wavefront's shade kernel and the raster pixel stage share the body, so they read the same emission.
    CHECK(openpbr.shade_source.contains("surface.emission_luminance = max(0.0, emission_luminance)"));

    // No scale attribute is a scale of 1.
    auto const pbr = generate(lib, sv::builtin_material::pbr, {});
    CHECK(pbr.source.contains("surface.emission_color = max(float3(0.0), emissive)"));
    CHECK(pbr.source.contains("surface.emission_luminance = 1.0"));

    // A type declaring none is left to its fragment.
    auto const unlit = generate(lib, sv::builtin_material::unlit, {});
    CHECK(!unlit.source.contains("the declared emission"));
}

TEST("sv - an emission descriptor says where each of its factors lives")
{
    auto lib = make_library();

    SECTION("constants, the signature's own defaults included, live in the parameter block")
    {
        auto const g = generate(lib, sv::builtin_material::openpbr, {});
        CHECK(g.emission.declares());
        CHECK(g.emission.is_constant());
        CHECK(g.emission.color.kind == sv::material_opacity_kind::constant);
        CHECK(g.emission.scale.kind == sv::material_opacity_kind::constant);
        auto color_offset = -1;
        auto scale_offset = -1;
        for (auto const& s : g.layout.slots)
        {
            if (s.name == "emission_color")
                color_offset = s.offset;
            if (s.name == "emission_luminance")
                scale_offset = s.offset;
        }
        CHECK(i32(g.emission.color.offset) == color_offset);
        CHECK(i32(g.emission.scale.offset) == scale_offset);
    }

    SECTION("a type with no scale declares none")
    {
        auto const e = emission_of(lib, sv::builtin_material::pbr, {});
        CHECK(e.color.is_constant());
        CHECK(e.scale.kind == sv::material_opacity_kind::none);
        CHECK(e.is_constant());
    }

    SECTION("a mesh attribute is read through its descriptor, which next-event estimation does not sample yet")
    {
        auto mesh = make_mesh();
        mesh.attributes.push_back(bind(sv::mesh_attribute::create("emissive", sv::attribute_frequency::per_triangle,
                                                                  cc::array<tg::vec3f>{tg::vec3f(1, 1, 1)})));
        auto const e = emission_of(lib, sv::builtin_material::pbr, {}, mesh);
        CHECK(e.color.kind == sv::material_opacity_kind::attribute);
        CHECK(e.color.frequency == 0u); // `material.frequency_per_triangle`
        CHECK(!e.is_constant());
    }

    SECTION("a texture keeps its uv, its sampler and the channels it reads")
    {
        auto mesh = make_mesh();
        mesh.attributes.push_back(
            bind(sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex,
                                            cc::array<tg::vec2f>{tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1)})));
        mesh.textures.push_back({.name = "emissive",
                                 .source = {.texture = sv::texture_id(1),
                                            .uv_attribute = "uv",
                                            .swizzle = sv::channel_swizzle::of(
                                                sv::texture_channel::b, sv::texture_channel::g, sv::texture_channel::r)}});
        auto const e = emission_of(lib, sv::builtin_material::pbr, {}, mesh);
        CHECK(e.color.kind == sv::material_opacity_kind::texture);
        CHECK(e.color.uv_offset != sv::material_opacity::no_offset);
        CHECK(e.color.channels[0] == 2u);
        CHECK(e.color.channels[1] == 1u);
        CHECK(e.color.channels[2] == 0u);
        CHECK(!e.is_constant());
    }
}

TEST("sv - an instance qualifies as a light by a constant emission above zero")
{
    auto lib = make_library();
    auto const radiance = [&](cc::string_view type, cc::vector<sv::material_attribute_binding> overrides,
                              sv::resident_mesh const& mesh = make_mesh())
    {
        auto copy = overrides;
        auto const emission = emission_of(lib, type, cc::move(copy), mesh);
        return sv::impl::emitter_radiance_of(emission, block_of(lib, type, cc::move(overrides), mesh));
    };

    SECTION("openpbr's default luminance is 0, so a material that sets none lights nothing")
    {
        CHECK(!radiance(sv::builtin_material::openpbr, {}).has_value());
    }

    SECTION("a positive one lights, each factor clamped at 0 as the generated emission is")
    {
        auto const r = radiance(sv::builtin_material::openpbr,
                                {sv::material_attribute_binding::of("emission_luminance", 2.0f),
                                 sv::material_attribute_binding::of("emission_color", tg::vec3f(1.0f, 0.5f, -1.0f))});
        REQUIRE(r.has_value());
        CHECK(r.value() == tg::vec3f(2.0f, 1.0f, 0.0f));
    }

    SECTION("a negative scale lights nothing")
    {
        CHECK(!radiance(sv::builtin_material::openpbr, {sv::material_attribute_binding::of("emission_luminance", -3.0f)})
                   .has_value());
    }

    SECTION("pbr's emissive lights at a scale of 1, and its black default lights nothing")
    {
        auto const r
            = radiance(sv::builtin_material::pbr, {sv::material_attribute_binding::of("emissive", tg::vec3f(3, 4, 5))});
        REQUIRE(r.has_value());
        CHECK(r.value() == tg::vec3f(3, 4, 5));
        CHECK(!radiance(sv::builtin_material::pbr, {}).has_value());
    }

    SECTION("an emission from a mesh attribute is camera-only until the reader samples it")
    {
        auto mesh = make_mesh();
        mesh.attributes.push_back(bind(sv::mesh_attribute::create("emissive", sv::attribute_frequency::per_triangle,
                                                                  cc::array<tg::vec3f>{tg::vec3f(1, 1, 1)})));
        CHECK(!radiance(sv::builtin_material::pbr, {}, mesh).has_value());
    }

    SECTION("unlit declares no emission, so however bright it is it lights nothing")
    {
        CHECK(!radiance(sv::builtin_material::unlit, {sv::material_attribute_binding::of("color", tg::vec3f(10, 10, 10))})
                   .has_value());
    }
}

TEST("sv - emissive instances' records follow every analytic light, and are picked by power")
{
    auto const rect = sv::shaders::tracer::light_record{.path = u32(sv::light_path::area),
                                                        .area = 1.0f,
                                                        .emission = tg::vec3f(1, 1, 1),
                                                        .cone_offset = 1};
    auto const sun
        = sv::shaders::tracer::light_record{.path = u32(sv::light_path::distant_point), .emission = tg::vec3f(1, 1, 1)};
    // A mesh record emits from both faces of its area, which is what its weight counts.
    auto const mesh = [](f32 tag)
    {
        return sv::shaders::tracer::light_record{.position = tg::vec3f(tag, 0, 0),
                                                 .path = sv::light_path_mesh,
                                                 .area = 1.0f,
                                                 .flags = sv::light_flag_two_sided,
                                                 .emission = tg::vec3f(1, 1, 1)};
    };

    auto const given = cc::vector<sv::shaders::tracer::light_record>{mesh(0), rect, mesh(1), sun};
    auto const table = sv::pt_light_table::grouped(given);

    // area, then distant_point, then the two mesh records in the order given.
    REQUIRE(table.records.size() == 4);
    CHECK(table.path_offset[sv::light_path_mesh] == 2);
    CHECK(table.path_count[sv::light_path_mesh] == 2);
    CHECK(table.records[2].path == sv::light_path_mesh);
    CHECK(table.records[2].position[0] == 0.0f);
    CHECK(table.records[3].position[0] == 1.0f);

    auto fc = sv::default_frame_constants();
    table.describe_in(fc);
    CHECK(fc.light_count == 4);
    CHECK(fc.emitter_offset == 2);

    // Local lights share half the choice with the sun; within it a two-sided mesh record weighs twice a one-sided rect.
    CHECK(tg::abs(table.records[0].select_pdf - 0.1f) < 1e-6f);
    CHECK(tg::abs(table.records[2].select_pdf - 0.2f) < 1e-6f);
    CHECK(tg::abs(table.records[3].select_pdf - 0.2f) < 1e-6f);
    CHECK(tg::abs(table.records[1].select_pdf - 0.5f) < 1e-6f);
    CHECK(table.records.back().select_cdf == 1.0f);

    SECTION("a table of emitters alone starts them at 0")
    {
        auto const alone = sv::pt_light_table::grouped(cc::vector<sv::shaders::tracer::light_record>{mesh(0)});
        auto fc_alone = sv::default_frame_constants();
        alone.describe_in(fc_alone);
        CHECK(fc_alone.emitter_offset == 0);
        CHECK(alone.records[0].select_pdf == 1.0f);
    }
}

TEST("sv - an emitter's world area follows its placement, exactly under a similarity")
{
    auto const rotation = tg::affine_transform3f::make_rotation(
        tg::quat_f::make_rotation_axis_angle(tg::normalize(tg::vec3f(1, 2, 3)), tg::angle_f::make_from_degree(40.0f)));
    CHECK(tg::abs(sv::impl::world_area_of(3.0f, rotation) - 3.0f) < 1e-4f);

    auto const scaled = tg::compose(tg::affine_transform3f::make_translation(tg::vec3f(5, 0, 0)),
                                    tg::affine_transform3f::make_scaling(tg::vec3f(2, 2, 2)));
    CHECK(tg::abs(sv::impl::world_area_of(3.0f, scaled) - 12.0f) < 1e-4f);

    // A non-uniform scale has no one factor; the estimate is the cube root of the volume's, squared.
    auto const stretched = tg::affine_transform3f::make_scaling(tg::vec3f(8, 1, 1));
    CHECK(tg::abs(sv::impl::world_area_of(1.0f, stretched) - 4.0f) < 1e-4f);
}
