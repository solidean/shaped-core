#include <clean-core/container/array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <nexus/test.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/resolve.hh>
#include <shaped-viewer/material/shader_generator.hh>
#include <shaped-viewer/rendering/impl/emitters.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/rendering/wavefront_routine.hh>
#include <shaped-viewer/resources/instance_data.hh>
#include <shaped-viewer/resources/resource_managers.hh> // sv::residency
#include <shaped-viewer/resources/value_summary.hh>
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

/// The parameter block of `type_name` under `overrides` as the resource manager keeps it, by the layout.
/// Constants and sample transforms carry their bytes, and a texture slot its id; a descriptor slot names no uploaded attribute.
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
        auto const& a = resolved.attributes[slot.attribute_index];
        if (slot.kind == sv::material_slot_kind::constant)
            s.constant = cc::vector<byte>::create_copy_of(a.constant);
        if (slot.kind == sv::material_slot_kind::sample_transform)
        {
            auto const t = a.sample->transform;
            s.constant = cc::vector<byte>::create_copy_of(cc::span<sv::sample_transform const>(&t, 1).as_bytes());
        }
        if (slot.kind == sv::material_slot_kind::texture_index)
            s.texture = a.sample->texture;
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

namespace
{
/// A mesh offering a per-vertex `uv` and the texture `texture` as `name`, read through `swizzle` and `transform`.
[[nodiscard]] sv::resident_mesh textured_mesh(cc::string name,
                                              sv::texture_id texture,
                                              sv::channel_swizzle swizzle = {},
                                              sv::sample_transform transform = {})
{
    auto mesh = make_mesh();
    mesh.attributes.push_back(
        bind(sv::mesh_attribute::create("uv", sv::attribute_frequency::per_vertex,
                                        cc::array<tg::vec2f>{tg::vec2f(0, 0), tg::vec2f(1, 0), tg::vec2f(0, 1)})));
    mesh.textures.push_back(
        {.name = cc::move(name),
         .source = {.texture = texture, .uv_attribute = "uv", .swizzle = swizzle, .transform = transform}});
    return mesh;
}

/// A mesh carrying `name` per triangle, one value for its one triangle.
[[nodiscard]] sv::resident_mesh attribute_mesh(cc::string name, tg::vec3f value)
{
    auto mesh = make_mesh();
    mesh.attributes.push_back(bind(sv::mesh_attribute::create(cc::move(name), sv::attribute_frequency::per_triangle,
                                                              cc::array<tg::vec3f>{value})));
    return mesh;
}

/// The summary of values whose mean is `mean` and range `lo` to `hi` in every color channel, alpha 1.
[[nodiscard]] sv::value_summary summary_of(f32 mean, f32 lo, f32 hi, bool exact)
{
    return {.mean = tg::vec4f(mean, mean, mean, 1),
            .min = tg::vec4f(lo, lo, lo, 1),
            .max = tg::vec4f(hi, hi, hi, 1),
            .exact = exact};
}
} // namespace

TEST("sv - an emission descriptor says where each of its factors lives")
{
    auto lib = make_library();

    SECTION("constants, the signature's own defaults included, live in the parameter block")
    {
        auto const g = generate(lib, sv::builtin_material::openpbr, {});
        CHECK(g.emission.declares());
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
    }

    SECTION("a mesh attribute is read through its descriptor, at its frequency")
    {
        auto const e = emission_of(lib, sv::builtin_material::pbr, {}, attribute_mesh("emissive", tg::vec3f(1, 1, 1)));
        CHECK(e.color.kind == sv::material_opacity_kind::attribute);
        CHECK(e.color.frequency == 0u); // `material.frequency_per_triangle`
        CHECK(e.color.offset != sv::material_opacity::no_offset);
    }

    SECTION("a texture keeps its uv, its sampler, the channels it reads and its transform")
    {
        auto const mesh = textured_mesh(
            "emissive", sv::texture_id(1),
            sv::channel_swizzle::of(sv::texture_channel::b, sv::texture_channel::g, sv::texture_channel::r),
            {.scale = tg::vec4f(2, 2, 2, 1), .bias = tg::vec4f(0, 0, 0, 0)});
        auto const g = generate(lib, sv::builtin_material::pbr, {}, mesh);
        auto const& e = g.emission;
        CHECK(e.color.kind == sv::material_opacity_kind::texture);
        CHECK(e.color.uv_offset != sv::material_opacity::no_offset);
        CHECK(e.color.frequency == 1u); // `material.frequency_per_vertex`, the uv's
        CHECK(e.color.channels[0] == 2u);
        CHECK(e.color.channels[1] == 1u);
        CHECK(e.color.channels[2] == 0u);
        CHECK(e.color.transform != sv::material_opacity::no_offset);
        // The generated hit and shade kernel sample at level 0, the level the light sampler's reader samples at.
        CHECK(g.source.contains("level = 0.0"));
        CHECK(g.shade_source.contains("level = 0.0"));
    }

    SECTION("an emitter record carries both factors, the color's three selectors packed a byte each")
    {
        auto const mesh = textured_mesh(
            "emissive", sv::texture_id(1),
            sv::channel_swizzle::of(sv::texture_channel::b, sv::texture_channel::one, sv::texture_channel::r));
        auto const textured = emission_of(lib, sv::builtin_material::pbr, {}, mesh);
        auto const r = sv::emitter_record_of(7, 9, 300, 2, textured);
        CHECK(r.instance == 7u);
        CHECK(r.cdf == 9u);
        CHECK(r.triangle_count == 300u);
        CHECK(r.block_count == 2u);
        CHECK(r.color_kind == u32(sv::material_opacity_kind::texture));
        CHECK(r.color_offset == textured.color.offset);
        CHECK(r.color_uv_offset == textured.color.uv_offset);
        CHECK(r.color_frequency == textured.color.frequency);
        CHECK(r.color_channels == (2u | (5u << 8)));
        CHECK(r.color_sampler == textured.color.sampler);
        CHECK(r.color_transform == sv::material_opacity::no_offset);
        CHECK(r.scale_kind == u32(sv::material_opacity_kind::none));

        auto const openpbr = emission_of(lib, sv::builtin_material::openpbr, {});
        auto const o = sv::emitter_record_of(0, 0, 1, 1, openpbr);
        CHECK(o.scale_kind == u32(sv::material_opacity_kind::constant));
        CHECK(o.scale_offset == openpbr.scale.offset);
        CHECK(o.scale_channel == 0u);
    }
}

TEST("sv - an instance qualifies as a light unless its emission is dark wherever it is read")
{
    auto lib = make_library();
    auto texture = cc::optional<sv::value_summary>();
    auto attribute = cc::optional<sv::value_summary>();
    auto texture_source = [&](sv::texture_id) { return texture; };
    auto attribute_source = [&](sv::attribute_id) { return attribute; };
    auto const estimate_of
        = [&](cc::string_view type, cc::vector<sv::material_attribute_binding> overrides, sv::resident_mesh const& mesh)
    {
        auto copy = overrides;
        auto const emission = emission_of(lib, type, cc::move(copy), mesh);
        return sv::impl::emitter_estimate_of(emission, block_of(lib, type, cc::move(overrides), mesh),
                                             {.texture = texture_source, .attribute = attribute_source});
    };
    // The radiance alone, which most sections ask about.
    auto const estimate = [&](cc::string_view type, cc::vector<sv::material_attribute_binding> overrides,
                              sv::resident_mesh const& mesh = make_mesh())
    {
        auto const e = estimate_of(type, cc::move(overrides), mesh);
        return e.has_value() ? cc::optional<tg::vec3f>(e.value().radiance) : cc::optional<tg::vec3f>();
    };

    SECTION("openpbr's default luminance is 0, so a material that sets none lights nothing")
    {
        CHECK(!estimate(sv::builtin_material::openpbr, {}).has_value());
    }

    SECTION("a positive constant lights, each factor clamped at 0 as the generated emission is")
    {
        auto const r = estimate(sv::builtin_material::openpbr,
                                {sv::material_attribute_binding::of("emission_luminance", 2.0f),
                                 sv::material_attribute_binding::of("emission_color", tg::vec3f(1.0f, 0.5f, -1.0f))});
        REQUIRE(r.has_value());
        CHECK(r.value() == tg::vec3f(2.0f, 1.0f, 0.0f));
    }

    SECTION("a negative constant scale lights nothing")
    {
        CHECK(!estimate(sv::builtin_material::openpbr, {sv::material_attribute_binding::of("emission_luminance", -3.0f)})
                   .has_value());
    }

    SECTION("pbr's emissive lights at a scale of 1, and its black default lights nothing")
    {
        auto const r
            = estimate(sv::builtin_material::pbr, {sv::material_attribute_binding::of("emissive", tg::vec3f(3, 4, 5))});
        REQUIRE(r.has_value());
        CHECK(r.value() == tg::vec3f(3, 4, 5));
        CHECK(!estimate(sv::builtin_material::pbr, {}).has_value());
    }

    SECTION("a mesh attribute lights at the mean of its elements")
    {
        attribute = summary_of(0.25f, 0.0f, 1.0f, true);
        auto const r = estimate(sv::builtin_material::pbr, {}, attribute_mesh("emissive", tg::vec3f(1, 1, 1)));
        REQUIRE(r.has_value());
        CHECK(r.value() == tg::vec3f(0.25f, 0.25f, 0.25f));
    }

    SECTION("a mesh attribute that is zero in every element lights nothing, as the test Cornell box's walls do not")
    {
        attribute = summary_of(0.0f, 0.0f, 0.0f, true);
        CHECK(!estimate(sv::builtin_material::pbr, {}, attribute_mesh("emissive", tg::vec3f(0, 0, 0))).has_value());
    }

    SECTION("a mesh attribute nothing is known of counts as 1, times the constant scale")
    {
        attribute = cc::nullopt;
        auto const r
            = estimate(sv::builtin_material::openpbr, {sv::material_attribute_binding::of("emission_luminance", 3.0f)},
                       attribute_mesh("emission_color", tg::vec3f(1, 1, 1)));
        REQUIRE(r.has_value());
        CHECK(r.value() == tg::vec3f(3, 3, 3));
    }

    SECTION("a texture lights at its mean, through its swizzle and its scale and bias")
    {
        texture = sv::value_summary{.mean = tg::vec4f(0.1f, 0.2f, 0.4f, 1.0f),
                                    .min = tg::vec4f(0, 0, 0, 1),
                                    .max = tg::vec4f(1, 1, 1, 1),
                                    .exact = true};
        auto const mesh = textured_mesh(
            "emissive", sv::texture_id(1),
            sv::channel_swizzle::of(sv::texture_channel::b, sv::texture_channel::one, sv::texture_channel::r),
            {.scale = tg::vec4f(2, 3, 4, 1), .bias = tg::vec4f(0.5f, 0, 0, 0)});
        auto const r = estimate(sv::builtin_material::pbr, {}, mesh);
        REQUIRE(r.has_value());
        CHECK(tg::abs(r.value()[0] - (0.4f * 2 + 0.5f)) < 1e-6f);
        CHECK(tg::abs(r.value()[1] - 3.0f) < 1e-6f);
        CHECK(tg::abs(r.value()[2] - 0.1f * 4) < 1e-6f);
    }

    SECTION("a texture read whole and black throughout lights nothing")
    {
        texture = summary_of(0.0f, 0.0f, 0.0f, true);
        CHECK(!estimate(sv::builtin_material::pbr, {}, textured_mesh("emissive", sv::texture_id(1))).has_value());
    }

    SECTION("a texture whose strided scan read only black still lights, at a weight above zero but tiny")
    {
        // As an importer's black emission map: nothing proves it dark, and nothing says it emits much.
        texture = summary_of(0.0f, 0.0f, 0.0f, false);
        auto const r = estimate_of(sv::builtin_material::pbr, {}, textured_mesh("emissive", sv::texture_id(1)));
        REQUIRE(r.has_value());
        for (auto c = 0; c < 3; ++c)
        {
            CHECK(r.value().radiance[c] > 0.0f);
            CHECK(r.value().radiance[c] < 1e-3f);
        }
        // A stand-in is no evidence of light, which selection is told.
        CHECK(r.value().unseen);
    }

    SECTION("an estimate from means or bounds that saw light is not a stand-in")
    {
        texture = summary_of(0.25f, 0.0f, 1.0f, false);
        auto const by_mean = estimate_of(sv::builtin_material::pbr, {}, textured_mesh("emissive", sv::texture_id(1)));
        REQUIRE(by_mean.has_value());
        CHECK(!by_mean.value().unseen);

        // A bias that clamps the mean to zero leaves the bound, which saw light.
        auto const mesh = textured_mesh("emissive", sv::texture_id(1), {},
                                        {.scale = tg::vec4f(1, 1, 1, 1), .bias = tg::vec4f(-0.5f, -0.5f, -0.5f, 0)});
        auto const by_bound = estimate_of(sv::builtin_material::pbr, {}, mesh);
        REQUIRE(by_bound.has_value());
        CHECK(!by_bound.value().unseen);
    }

    SECTION("a bias that cancels the brightest texel to about zero proves nothing, while one well below it does")
    {
        // Texels up to 1 under a scale of 2: a bias of -2 meets the bound at exactly 0 on the CPU, which the GPU's rounding may leave just above.
        texture = summary_of(0.5f, 0.0f, 1.0f, true);
        auto const at_zero = textured_mesh("emissive", sv::texture_id(1), {},
                                           {.scale = tg::vec4f(2, 2, 2, 1), .bias = tg::vec4f(-2, -2, -2, 0)});
        auto const r = estimate_of(sv::builtin_material::pbr, {}, at_zero);
        REQUIRE(r.has_value());
        CHECK(r.value().unseen);

        // Within the margin below zero is no proof either.
        auto const just_below
            = textured_mesh("emissive", sv::texture_id(1), {},
                            {.scale = tg::vec4f(2, 2, 2, 1), .bias = tg::vec4f(-2.001f, -2.001f, -2.001f, 0)});
        CHECK(estimate(sv::builtin_material::pbr, {}, just_below).has_value());

        // Well below zero is dark.
        auto const below = textured_mesh("emissive", sv::texture_id(1), {},
                                         {.scale = tg::vec4f(2, 2, 2, 1), .bias = tg::vec4f(-2.1f, -2.1f, -2.1f, 0)});
        CHECK(!estimate(sv::builtin_material::pbr, {}, below).has_value());

        // With no bias the bound is a product, which the GPU rounds to zero as this does.
        texture = summary_of(0.0f, 0.0f, 0.0f, true);
        auto const unbiased = textured_mesh("emissive", sv::texture_id(1), {},
                                            {.scale = tg::vec4f(2, 2, 2, 1), .bias = tg::vec4f(0, 0, 0, 0)});
        CHECK(!estimate(sv::builtin_material::pbr, {}, unbiased).has_value());
    }

    SECTION("a texture whose mean clamps to zero but whose range does not is weighed by its bound")
    {
        // A bias of -0.5 over texels in [0, 1] averaging 0.25: dark on average, and bright where a texel is above a half.
        texture = summary_of(0.25f, 0.0f, 1.0f, true);
        auto const mesh = textured_mesh("emissive", sv::texture_id(1), {},
                                        {.scale = tg::vec4f(1, 1, 1, 1), .bias = tg::vec4f(-0.5f, -0.5f, -0.5f, 0)});
        auto const r = estimate(sv::builtin_material::pbr, {}, mesh);
        REQUIRE(r.has_value());
        CHECK(r.value() == tg::vec3f(0.5f, 0.5f, 0.5f));
    }

    SECTION("a texture nothing is known of counts as 1")
    {
        texture = cc::nullopt;
        auto const r = estimate(sv::builtin_material::pbr, {}, textured_mesh("emissive", sv::texture_id(1)));
        REQUIRE(r.has_value());
        CHECK(r.value() == tg::vec3f(1, 1, 1));
    }

    SECTION("a textured color under a zero constant scale lights nothing")
    {
        texture = summary_of(0.5f, 0.0f, 1.0f, true);
        CHECK(!estimate(sv::builtin_material::openpbr, {}, textured_mesh("emission_color", sv::texture_id(1))).has_value());
    }

    SECTION("a texture nothing is known of reads as 1 through its swizzle and its scale and bias")
    {
        texture = cc::nullopt;
        auto const mesh = textured_mesh(
            "emissive", sv::texture_id(1),
            sv::channel_swizzle::of(sv::texture_channel::r, sv::texture_channel::zero, sv::texture_channel::g),
            {.scale = tg::vec4f(2, 2, 3, 1), .bias = tg::vec4f(0.5f, 0.25f, -1, 0)});
        auto const r = estimate(sv::builtin_material::pbr, {}, mesh);
        REQUIRE(r.has_value());
        CHECK(r.value() == tg::vec3f(2.5f, 0.25f, 2.0f));
    }

    SECTION("a mean that clamps to zero gives way to an inexact bound above zero, before the unknown 1")
    {
        // A strided scan that averaged below the bias but saw texels above it.
        texture = summary_of(0.25f, 0.0f, 0.8f, false);
        auto const mesh = textured_mesh("emissive", sv::texture_id(1), {},
                                        {.scale = tg::vec4f(1, 1, 1, 1), .bias = tg::vec4f(-0.5f, -0.5f, -0.5f, 0)});
        auto const r = estimate(sv::builtin_material::pbr, {}, mesh);
        REQUIRE(r.has_value());
        CHECK(tg::abs(r.value()[0] - 0.3f) < 1e-6f);
        CHECK(tg::abs(r.value()[2] - 0.3f) < 1e-6f);
    }

    SECTION("a factor whose inexact summary saw nothing above zero stands in tiny, beside a factor that keeps its "
            "value")
    {
        // A textured scale whose strided scan read only black, under a constant color that keeps its own value.
        texture = summary_of(0.0f, 0.0f, 0.0f, false);
        auto const r = estimate(sv::builtin_material::openpbr,
                                {sv::material_attribute_binding::of("emission_color", tg::vec3f(2, 3, 4))},
                                textured_mesh("emission_luminance", sv::texture_id(1)));
        REQUIRE(r.has_value());
        CHECK(r.value()[0] > 0.0f);
        CHECK(r.value()[0] < 1e-3f);
        // The color's proportions survive the stand-in.
        CHECK(tg::abs(r.value()[1] / r.value()[0] - 1.5f) < 1e-4f);
        CHECK(tg::abs(r.value()[2] / r.value()[0] - 2.0f) < 1e-4f);
    }

    SECTION("a lane under a transform scale of 0 reads its bias alone, whatever the texture holds")
    {
        // Nothing is known of the texture, yet a scale of 0 and a bias at or below zero prove every lane dark.
        texture = cc::nullopt;
        auto const dark = textured_mesh("emissive", sv::texture_id(1), {},
                                        {.scale = tg::vec4f(0, 0, 0, 1), .bias = tg::vec4f(0, -1, 0, 0)});
        CHECK(!estimate(sv::builtin_material::pbr, {}, dark).has_value());

        // An inexact scan that saw bright texels changes nothing: the lanes do not read them.
        texture = summary_of(0.5f, 0.0f, 1.0f, false);
        CHECK(!estimate(sv::builtin_material::pbr, {}, dark).has_value());

        // A positive bias is the lane's exact value, and its estimate.
        auto const lit = textured_mesh("emissive", sv::texture_id(1), {},
                                       {.scale = tg::vec4f(0, 0, 0, 1), .bias = tg::vec4f(0.5f, 0, 0.25f, 0)});
        auto const r = estimate(sv::builtin_material::pbr, {}, lit);
        REQUIRE(r.has_value());
        CHECK(r.value() == tg::vec3f(0.5f, 0, 0.25f));

        // A textured scale of 0 and a bias of -1 is a scale dark throughout, under however bright a color.
        auto const scale = textured_mesh("emission_luminance", sv::texture_id(1), {},
                                         {.scale = tg::vec4f(0, 1, 1, 1), .bias = tg::vec4f(-1, 0, 0, 0)});
        CHECK(!estimate(sv::builtin_material::openpbr,
                        {sv::material_attribute_binding::of("emission_color", tg::vec3f(10, 10, 10))}, scale)
                   .has_value());
    }

    SECTION("only every lane known proves a texture dark: one the texels decide still lights")
    {
        // Red and green under a scale of 0 and no bias, blue read through a scan that saw only black.
        texture = summary_of(0.0f, 0.0f, 0.0f, false);
        auto const mesh = textured_mesh("emissive", sv::texture_id(1), {},
                                        {.scale = tg::vec4f(0, 0, 1, 1), .bias = tg::vec4f(0, 0, 0, 0)});
        auto const r = estimate(sv::builtin_material::pbr, {}, mesh);
        REQUIRE(r.has_value());
        CHECK(r.value()[2] > 0.0f);

        // Once that scan is exact, every lane is known, and dark.
        texture = summary_of(0.0f, 0.0f, 0.0f, true);
        CHECK(!estimate(sv::builtin_material::pbr, {}, mesh).has_value());
    }

    SECTION("a swizzle of constant zeros is dark whatever the texture holds")
    {
        texture = cc::nullopt;
        auto const mesh = textured_mesh(
            "emissive", sv::texture_id(1),
            sv::channel_swizzle::of(sv::texture_channel::zero, sv::texture_channel::zero, sv::texture_channel::zero));
        CHECK(!estimate(sv::builtin_material::pbr, {}, mesh).has_value());
    }

    SECTION("a non-finite constant or transform lights nothing, rather than poisoning the selection CDF")
    {
        auto const nan = cc::bit_cast<f32>(0x7fc00000u);
        auto const inf = cc::bit_cast<f32>(0x7f800000u);
        CHECK(!estimate(sv::builtin_material::pbr, {sv::material_attribute_binding::of("emissive", tg::vec3f(nan, 1, 1))})
                   .has_value());
        CHECK(!estimate(sv::builtin_material::pbr, {sv::material_attribute_binding::of("emissive", tg::vec3f(inf, 1, 1))})
                   .has_value());
        CHECK(!estimate(sv::builtin_material::openpbr, {sv::material_attribute_binding::of("emission_luminance", nan)})
                   .has_value());

        texture = summary_of(0.5f, 0.0f, 1.0f, true);
        auto const mesh = textured_mesh("emissive", sv::texture_id(1), {},
                                        {.scale = tg::vec4f(1, nan, 1, 1), .bias = tg::vec4f(0, 0, 0, 0)});
        CHECK(!estimate(sv::builtin_material::pbr, {}, mesh).has_value());
    }

    SECTION("finite factors whose product overflows give a finite estimate")
    {
        auto const r = estimate(sv::builtin_material::openpbr,
                                {sv::material_attribute_binding::of("emission_luminance", 1e30f),
                                 sv::material_attribute_binding::of("emission_color", tg::vec3f(1e30f, 1, 0))});
        REQUIRE(r.has_value());
        CHECK(tg::is_finite(r.value()[0]));
        CHECK(r.value()[0] > 1e38f);
        CHECK(r.value()[1] == 1e30f);
    }

    SECTION("unlit declares no emission, so however bright it is it lights nothing")
    {
        CHECK(!estimate(sv::builtin_material::unlit, {sv::material_attribute_binding::of("color", tg::vec3f(10, 10, 10))})
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

    SECTION("an emitter whose scan saw only black is picked rarely, at the floor of its kind, yet never not at all")
    {
        auto lib = make_library();
        auto texture_source = [](sv::texture_id) { return cc::optional<sv::value_summary>(summary_of(0, 0, 0, false)); };
        auto attribute_source = [](sv::attribute_id) { return cc::optional<sv::value_summary>(); };
        auto const black_map = textured_mesh("emissive", sv::texture_id(1));
        auto const estimate = sv::impl::emitter_estimate_of(emission_of(lib, sv::builtin_material::pbr, {}, black_map),
                                                            block_of(lib, sv::builtin_material::pbr, {}, black_map),
                                                            {.texture = texture_source, .attribute = attribute_source});
        REQUIRE(estimate.has_value());
        REQUIRE(estimate.value().unseen);

        auto dim = mesh(0);
        dim.emission = estimate.value().radiance;
        dim.flags |= sv::light_flag_unseen_emission;
        auto const with_lamp = sv::pt_light_table::grouped(cc::vector<sv::shaders::tracer::light_record>{dim, mesh(1)});
        REQUIRE(with_lamp.records.size() == 2);
        // The floor is a thousandth of the kind's mean weight, here half the lamp's.
        CHECK(with_lamp.records[0].select_pdf > 0.0f);
        CHECK(tg::abs(with_lamp.records[0].select_pdf - 5e-4f) < 1e-5f);

        // Beside a sun, local lights made only of stand-ins take a sliver of the choice rather than half of it.
        auto const with_sun = sv::pt_light_table::grouped(cc::vector<sv::shaders::tracer::light_record>{dim, sun});
        REQUIRE(with_sun.records.size() == 2);
        REQUIRE(with_sun.records[1].path == sv::light_path_mesh);
        CHECK(with_sun.records[1].select_pdf > 0.0f);
        CHECK(with_sun.records[1].select_pdf < 2e-3f);
        CHECK(with_sun.records[0].select_pdf > 0.998f);

        // A lamp among them is light again, and the local kind takes its half.
        auto const lamp_and_sun
            = sv::pt_light_table::grouped(cc::vector<sv::shaders::tracer::light_record>{dim, mesh(1), sun});
        REQUIRE(lamp_and_sun.records.size() == 3);
        CHECK(tg::abs(lamp_and_sun.records[0].select_pdf - 0.5f) < 1e-6f);

        // Alone, stand-ins take the whole choice: nothing else may emit.
        auto const alone = sv::pt_light_table::grouped(cc::vector<sv::shaders::tracer::light_record>{dim});
        CHECK(alone.records[0].select_pdf == 1.0f);
    }

    SECTION("a table of emitters alone starts them at 0")
    {
        auto const alone = sv::pt_light_table::grouped(cc::vector<sv::shaders::tracer::light_record>{mesh(0)});
        auto fc_alone = sv::default_frame_constants();
        alone.describe_in(fc_alone);
        CHECK(fc_alone.emitter_offset == 0);
        CHECK(alone.records[0].select_pdf == 1.0f);
    }
}

TEST("sv - a texture's summary weighs an emitter only once the texture samples its own texels")
{
    auto const black = summary_of(0.0f, 0.0f, 0.0f, true);
    // A pending texture samples a placeholder, and a failed one magenta: neither is what the summary read.
    CHECK(!sv::impl::sampled_texture_summary(sv::residency::pending, black).has_value());
    CHECK(!sv::impl::sampled_texture_summary(sv::residency::failed, black).has_value());
    REQUIRE(sv::impl::sampled_texture_summary(sv::residency::base_resident, black).has_value());
    CHECK(sv::impl::sampled_texture_summary(sv::residency::complete, black).value().exact);
    CHECK(!sv::impl::sampled_texture_summary(sv::residency::complete, cc::nullopt).has_value());
}

TEST("sv - a light whose weight is not finite still leaves a finite selection CDF")
{
    auto const nan = cc::bit_cast<f32>(0x7fc00000u);
    auto const record = [](tg::vec3f emission, f32 area)
    {
        return sv::shaders::tracer::light_record{.path = sv::light_path_mesh,
                                                 .area = area,
                                                 .flags = sv::light_flag_two_sided,
                                                 .emission = emission};
    };
    // An overflowing weight, a NaN one, and an ordinary one beside them.
    auto const given = cc::vector<sv::shaders::tracer::light_record>{record(tg::vec3f(1e30f, 1e30f, 1e30f), 1e30f),
                                                                     record(tg::vec3f(nan, 1, 1), 1.0f),
                                                                     record(tg::vec3f(1, 1, 1), 1.0f)};
    auto const table = sv::pt_light_table::grouped(given);
    REQUIRE(table.records.size() == 3);
    auto previous = 0.0f;
    for (auto const& l : table.records)
    {
        CHECK(tg::is_finite(l.select_pdf));
        CHECK(l.select_pdf > 0.0f);
        CHECK(l.select_cdf >= previous);
        previous = l.select_cdf;
    }
    CHECK(table.records.back().select_cdf == 1.0f);
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

// ReSTIR DI names a light by its index in the table and a triangle by its primitive, so its history is kept under the table's key.
// Any change to what an index names changes the key, and so does an emitter that moves.
// A reservoir keeps its point as barycentrics, and its weight holds only under the placement it was drawn under.
// Reusing it under another would need the history's resampling weight formed with the old placement, which the frame no longer has.
// How candidates are drawn does not change the key: the selection's probabilities, an emissive record's area and emission, and the area CDF's bindless index.
// An instance that lights nothing does not change it either, wherever it stands.
TEST("sv - a light table's key follows what its indices name and where its emitters stand, not how candidates are "
     "drawn")
{
    using records = cc::vector<sv::shaders::tracer::light_record>;
    using emitter_records = cc::vector<sv::shaders::tracer::emitter_record>;
    using instance_records = cc::vector<sv::shaders::tracer::instance_record>;

    auto const rect = sv::shaders::tracer::light_record{.path = u32(sv::light_path::area),
                                                        .area = 1.0f,
                                                        .emission = tg::vec3f(1, 1, 1),
                                                        .cone_offset = 1};
    auto const mesh = [](f32 area)
    {
        return sv::shaders::tracer::light_record{.path = sv::light_path_mesh,
                                                 .area = area,
                                                 .flags = sv::light_flag_two_sided,
                                                 .emission = tg::vec3f(1, 1, 1)};
    };
    auto const emitter = [](u32 instance, u32 cdf)
    {
        return sv::shaders::tracer::emitter_record{.instance = instance, .cdf = cdf, .triangle_count = 2, .block_count = 1};
    };
    // An instance record placed at `x` along the x axis, scaled by `scale` along it.
    auto const placed_at = [](f32 x, f32 scale)
    {
        return sv::shaders::tracer::instance_record{.to_world_x = tg::vec4f(scale, 0, 0, x),
                                                    .to_world_y = tg::vec4f(0, 1, 0, 0),
                                                    .to_world_z = tg::vec4f(0, 0, 1, 0)};
    };

    auto const table = sv::pt_light_table::grouped(records{rect, mesh(1), mesh(2)});
    auto const emitters = emitter_records{emitter(1, 7), emitter(2, 8)};
    auto const instances = instance_records{placed_at(0, 1), placed_at(1, 1), placed_at(2, 1), placed_at(3, 1)};
    auto const key = table.key(emitters, instances);

    CHECK(sv::pt_light_table::grouped(records{rect, mesh(1), mesh(2)}).key(emitters, instances) == key);
    CHECK(table.key(emitter_records{emitter(1, 70), emitter(2, 80)}, instances) == key);

    // A scaled or rotated emitter's area, which reweighs every record's selection, and its selection's estimate of its emission.
    CHECK(sv::pt_light_table::grouped(records{rect, mesh(1.0000001f), mesh(2)}).key(emitters, instances) == key);
    CHECK(sv::pt_light_table::grouped(records{rect, mesh(1), mesh(5)}).key(emitters, instances) == key);
    auto dimmer = mesh(1);
    dimmer.emission = tg::vec3f(0.5f, 0.5f, 0.5f);
    CHECK(sv::pt_light_table::grouped(records{rect, dimmer, mesh(2)}).key(emitters, instances) == key);

    // An instance that lights nothing moves freely.
    auto moved_other = instances;
    moved_other[0] = placed_at(5, 1);
    moved_other[3] = placed_at(5, 2);
    CHECK(table.key(emitters, moved_other) == key);

    // An emitter that moves, rigidly or not, changes it: a kept point is that sample only under the placement it was drawn under.
    auto moved = instances;
    moved[2] = placed_at(2.5f, 1);
    CHECK(table.key(emitters, moved) != key);
    auto stretched = instances;
    stretched[1] = placed_at(1, 2);
    CHECK(table.key(emitters, stretched) != key);

    // An emitter landing appends a record, so every later index may mean another light.
    auto const grown = sv::pt_light_table::grouped(records{rect, mesh(1), mesh(2), mesh(3)});
    CHECK(grown.key(emitter_records{emitter(1, 7), emitter(2, 8), emitter(3, 9)}, instances) != key);

    // Two emitters swapping their places in the table swap what their indices mean.
    auto const swapped = sv::pt_light_table::grouped(records{rect, mesh(2), mesh(1)});
    CHECK(swapped.key(emitter_records{emitter(2, 8), emitter(1, 7)}, instances) != key);

    // The same record naming another instance's row names another mesh's triangles.
    CHECK(table.key(emitter_records{emitter(1, 7), emitter(3, 8)}, instances) != key);

    // An analytic light that changes changes the key too.
    auto brighter = rect;
    brighter.emission = tg::vec3f(2, 2, 2);
    CHECK(sv::pt_light_table::grouped(records{brighter, mesh(1), mesh(2)}).key(emitters, instances) != key);
}

// A ReSTIR frame reuses GI's history under the camera its records were kept under, and DI's under the light table too.
// A camera that moved is no reason to drop either: the frame is told where it was, which is the camera the records were kept under.
TEST("sv::impl::restir_history_flags - DI history needs the light table it was kept under, and survives a camera move")
{
    auto const kept_under = sv::shaders::tracer::camera_record{.position = tg::vec3f(0, 0, -3)};
    auto const elsewhere = sv::shaders::tracer::camera_record{.position = tg::vec3f(1, 0, -3)};
    auto const gi = sv::impl::wavefront_flag_restir_history;
    auto const both = sv::impl::wavefront_flag_restir_history | sv::impl::wavefront_flag_restir_di_history;

    auto state = sv::wavefront_state{};
    state.restir_written = true;
    state.restir_di_written = true;
    state.restir_camera = kept_under;
    state.restir_di_lights = 42;
    auto const trace = sv::pt_trace_desc{};
    auto const desc = [&](u64 lights_key)
    {
        return sv::wavefront_trace_desc{.trace = trace,
                                        .state = &state,
                                        .restir = true,
                                        .restir_di = true,
                                        .lights_key = lights_key};
    };

    // The camera has moved since: this frame's camera is another, and its previous one is where the records were kept.
    CHECK(sv::impl::restir_history_flags(desc(42), kept_under) == both);
    CHECK(sv::impl::restir_history_flags(desc(43), kept_under) == gi);
    // Frames between moved the camera without keeping records, so nothing reprojects.
    CHECK(sv::impl::restir_history_flags(desc(42), elsewhere) == 0u);

    state.restir_di_written = false;
    CHECK(sv::impl::restir_history_flags(desc(42), kept_under) == gi);
    state.restir_di_written = true;
    state.restir_written = false;
    CHECK(sv::impl::restir_history_flags(desc(42), kept_under) == 0u);
}
