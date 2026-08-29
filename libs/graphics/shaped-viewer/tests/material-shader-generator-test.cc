#include <clean-core/container/array.hh>
#include <clean-core/container/vector.hh>
#include <nexus/test.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/resolve.hh>
#include <shaped-viewer/material/shader_generator.hh>
#include <shaped-viewer/resources/bindless_tables.hh>
#include <shaped-viewer/scene/mesh.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

// CPU-only tests for the material shader generator.
// No GPU and no compiler: what is checked here is the TEXT and the parameter layout, which is what decides whether two materials share a permutation.
// That a generated shader actually compiles is a separate, DXC-gated test.

namespace
{
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

/// A type with one f32 and one vec3f attribute, so both the scalar and the vector paths are covered.
[[nodiscard]] sv::material_type make_type()
{
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("roughness", 0.5f));
    signature.push_back(sv::material_signature_entry::of("base_color", tg::vec3f(0.8f, 0.8f, 0.8f)));
    return sv::material_type::create("test", cc::move(signature),
                                     "    surface.specular_roughness = roughness;\n    surface.base_color = "
                                     "base_color;");
}

[[nodiscard]] sv::material bare_material()
{
    return sv::material::create("m", sv::material_type_id(0), {});
}

[[nodiscard]] sv::material_slot const& slot_named(sv::material_parameter_layout const& l, cc::string_view name)
{
    for (auto const& s : l.slots)
        if (s.name == name)
            return s;
    CC_ASSERT(false, "no such slot");
    return l.slots[0];
}
} // namespace

TEST("sv::hlsl_type_of - the supported formats and the ones that are not")
{
    CHECK(sv::hlsl_type_of(sv::attribute_format_of<f32>) == "float");
    CHECK(sv::hlsl_type_of(sv::attribute_format_of<tg::vec2f>) == "float2");
    CHECK(sv::hlsl_type_of(sv::attribute_format_of<tg::vec3f>) == "float3");
    CHECK(sv::hlsl_type_of(sv::attribute_format_of<tg::vec4f>) == "float4");
    CHECK(sv::hlsl_type_of(sv::attribute_format_of<i32>) == "int");
    CHECK(sv::hlsl_type_of(sv::attribute_format_of<u32>) == "uint");

    // A matrix and the narrow / wide scalars have no settled ByteAddressBuffer layout here yet, and say so by mapping to nothing.
    CHECK(sv::hlsl_type_of(sv::attribute_format::of_matrix(sv::scalar_type::f32, 4, 4)).empty());
    CHECK(sv::hlsl_type_of(sv::attribute_format_of<f64>).empty());
    CHECK(sv::hlsl_type_of(sv::attribute_format_of<u8>).empty());
}

TEST("sv::generate_material_shader - constants come out of the parameter block")
{
    auto const type = make_type();
    auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), make_mesh()));

    CHECK(g.source.contains("sv::surface sv_evaluate_material(sv::shading_context ctx)"));
    CHECK(g.source.contains("#include \"material_runtime.hlsli\""));

    // One local per signature attribute, at the type the format names, filled from the block that holds the loads.
    CHECK(g.source.contains("float roughness = 0;"));
    CHECK(g.source.contains("float3 base_color = 0;"));
    CHECK(g.source.contains("ByteAddressBuffer params = gBindlessBuffers[NonUniformResourceIndex(ctx.param_buffer)];"));
    CHECK(g.source.contains("roughness = asfloat(params.Load(ctx.param_offset + 0));"));
    CHECK(g.source.contains("base_color = asfloat(params.Load3(ctx.param_offset + 4));"));

    // The fragment is emitted verbatim, after every local it reads.
    CHECK(g.source.contains("surface.specular_roughness = roughness;"));
    CHECK(g.source.find("float3 base_color") < g.source.find("surface.base_color = base_color;"));

    // No texture is sampled, so no texture table is declared at all — the reflection stays as small as the material is.
    CHECK(!g.source.contains("gBindlessTextures2D"));

    // Nothing supplied either attribute, so the fragment's defaults are what came through — and the epilogue can tell.
    CHECK(g.source.contains("#define SV_ATTR_SUPPLIED_roughness 0"));
    CHECK(g.source.contains("#define SV_ATTR_SUPPLIED_base_color 0"));
    CHECK(!g.source.contains("SamplerState"));
    CHECK(g.source.contains("ByteAddressBuffer gBindlessBuffers[4096] : register(t0, space8);"));

    // The layout the CPU fills is the one the source reads.
    CHECK(g.layout.slots.size() == 2);
    CHECK(slot_named(g.layout, "roughness").kind == sv::material_slot_kind::constant);
    CHECK(slot_named(g.layout, "roughness").offset == 0);
    CHECK(slot_named(g.layout, "base_color").offset == 4);
    CHECK(slot_named(g.layout, "base_color").size_bytes == 12);
    CHECK(g.layout.size_bytes == 16);

    // The key covers the resolution's shape AND how these options spell it, so it is deliberately not the permutation key.
    auto const resolved = sv::resolve_material(type, bare_material(), make_mesh());
    CHECK(g.key != resolved.permutation_key);
    CHECK(g.key == sv::material_shader_key(resolved.permutation_key, {}));

    // Same resolution, different spelling, different entry: two callers cannot collide on one cache entry.
    CHECK(sv::generate_material_shader(resolved, {.entry_point = "other"}).key != g.key);
    CHECK(sv::generate_material_shader(resolved, {.epilogue_include = "epilogue.hlsli"}).key != g.key);
    auto const smaller = sv::bindless_config{.tables = {{.table = sv::bindless_table::textures_2d, .count = 2},
                                                        {.table = sv::bindless_table::buffers, .count = 2}}};
    CHECK(sv::generate_material_shader(resolved, {.bindless = &smaller}).key != g.key);
}

TEST("sv::generate_material_shader - an attribute-less type still declares the buffer table")
{
    // The epilogue reads the mesh's positions through gBindlessBuffers whatever the signature holds,
    // so a type with no attributes at all would otherwise generate a shader that does not compile.
    auto const type = sv::material_type::create("bare", {}, "    surface.albedo = float3(1, 0, 1);");
    auto const resolved = sv::resolve_material(type, bare_material(), make_mesh());

    auto const with_epilogue = sv::generate_material_shader(resolved, {.epilogue_include = "epilogue.hlsli"});
    CHECK(with_epilogue.source.contains("ByteAddressBuffer gBindlessBuffers[4096] : register(t0, space8);"));
    CHECK(with_epilogue.layout.slots.empty());

    // Without one, nothing in the source reaches a buffer, and the declaration stays out.
    CHECK(!sv::generate_material_shader(resolved).source.contains("gBindlessBuffers"));
}

TEST("sv::generate_material_shader - a mesh attribute is loaded through its descriptor")
{
    auto const type = make_type();

    auto per_vertex = make_mesh();
    auto const values = cc::array<f32>{0.1f, 0.2f, 0.3f};
    per_vertex.attributes.push_back(sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_vertex, values));
    {
        auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), per_vertex));
        CHECK(g.source.contains("sv::attribute_desc desc = sv::load_attribute_desc(params, ctx.param_offset + 0);"));
        CHECK(g.source.contains(
            "roughness = sv::interpolate_f1(gBindlessBuffers[NonUniformResourceIndex(desc.buffer)], desc, ctx.corner, "
            "ctx.barycentrics);"));
        CHECK(slot_named(g.layout, "roughness").kind == sv::material_slot_kind::attribute_descriptor);
        CHECK(slot_named(g.layout, "roughness").size_bytes == 12);
    }

    // per_corner reads the same way but numbers its elements off the primitive rather than off the vertex indices.
    auto per_corner = make_mesh();
    per_corner.attributes.push_back(sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_corner, values));
    {
        auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), per_corner));
        CHECK(g.source.contains("sv::interpolate_f1"));
        CHECK(g.source.contains("sv::corner_elements(ctx)"));
    }

    // per_triangle is flat — one element for the whole primitive, so it loads rather than interpolates.
    auto per_triangle = make_mesh();
    per_triangle.attributes.push_back(
        sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_triangle, cc::array<f32>{0.4f}));
    {
        auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), per_triangle));
        CHECK(g.source.contains("sv::load_element_f1"));
        CHECK(!g.source.contains("sv::interpolate_f1"));
        CHECK(g.source.contains("ctx.primitive"));
    }
}

TEST("sv::generate_material_shader - a texture samples through its uv attribute")
{
    auto const type = make_type();

    auto mesh = make_mesh();
    mesh.attributes.push_back(make_uvs());
    mesh.textures.push_back({.name = "base_color", .source = {.texture = sv::texture_id(3), .uv_attribute = "uv"}});

    auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), mesh));

    CHECK(g.source.contains("Texture2D gBindlessTextures2D[4096] : register(t0, space3);"));
    CHECK(g.source.contains("SamplerState sv_sampler_0 : register(s0, space0);"));
    CHECK(g.source.contains("float2 uv = sv::interpolate_f2("));
    CHECK(g.source.contains("uint tex = params.Load(ctx.param_offset + "));
    // SampleLevel, because a ray tracing hit shader has no derivatives to pick a mip from.
    CHECK(g.source.contains("SampleLevel(sv_sampler_0, uv, 0).rgb;"));

    // A sampled attribute takes two slots: the texture index, and the uv descriptor it is sampled through.
    CHECK(slot_named(g.layout, "base_color").kind == sv::material_slot_kind::texture_index);
    CHECK(slot_named(g.layout, "base_color.uv").kind == sv::material_slot_kind::attribute_descriptor);
    CHECK(slot_named(g.layout, "base_color.uv").format == sv::attribute_format::of_vector(sv::scalar_type::f32, 2));
}

TEST("sv::generate_material_shader - the permutation split holds at the level of the text")
{
    auto const type = make_type();
    auto const mesh = make_mesh();

    auto gold_b = cc::vector<sv::material_attribute_binding>();
    gold_b.push_back(sv::material_attribute_binding::of("roughness", 0.2f));
    auto copper_b = cc::vector<sv::material_attribute_binding>();
    copper_b.push_back(sv::material_attribute_binding::of("roughness", 0.6f));

    auto const gold = sv::generate_material_shader(
        sv::resolve_material(type, sv::material::create("gold", sv::material_type_id(0), gold_b), mesh));
    auto const copper = sv::generate_material_shader(
        sv::resolve_material(type, sv::material::create("copper", sv::material_type_id(0), copper_b), mesh));

    // The whole point: two materials differing only in their constants generate BYTE-IDENTICAL source.
    CHECK(gold.source == copper.source);
    CHECK(gold.key == copper.key);

    // A uv set at a different frequency is a different load, so it must be a different permutation — which is why the uv
    // attribute's frequency is part of the key rather than of the parameters.
    auto per_vertex_uv = make_mesh();
    per_vertex_uv.attributes.push_back(make_uvs(sv::attribute_frequency::per_vertex));
    per_vertex_uv.textures.push_back(
        {.name = "base_color", .source = {.texture = sv::texture_id(3), .uv_attribute = "uv"}});

    auto per_corner_uv = make_mesh();
    per_corner_uv.attributes.push_back(make_uvs(sv::attribute_frequency::per_corner));
    per_corner_uv.textures.push_back(
        {.name = "base_color", .source = {.texture = sv::texture_id(3), .uv_attribute = "uv"}});

    auto const a = sv::generate_material_shader(sv::resolve_material(type, bare_material(), per_vertex_uv));
    auto const b = sv::generate_material_shader(sv::resolve_material(type, bare_material(), per_corner_uv));
    CHECK(a.key != b.key);
    CHECK(a.source != b.source);
}

TEST("sv::generate_material_shader - two attributes sampled alike share one sampler")
{
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("base_color", tg::vec3f(0.8f, 0.8f, 0.8f)));
    signature.push_back(sv::material_signature_entry::of("emissive", tg::vec3f(0.0f, 0.0f, 0.0f)));
    auto const type = sv::material_type::create("two", cc::move(signature), "    surface.base_color = base_color;");

    auto mesh = make_mesh();
    mesh.attributes.push_back(make_uvs());
    mesh.textures.push_back({.name = "base_color", .source = {.texture = sv::texture_id(1), .uv_attribute = "uv"}});
    mesh.textures.push_back({.name = "emissive", .source = {.texture = sv::texture_id(2), .uv_attribute = "uv"}});

    auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), mesh));
    CHECK(g.source.contains("SamplerState sv_sampler_0"));
    CHECK(!g.source.contains("SamplerState sv_sampler_1"));

    // Two textures, two uv descriptors — the texture id is a parameter, so the two ids do not multiply the permutations.
    CHECK(g.layout.slots.size() == 4);
}

TEST("sv::generate_material_shader - a rotation attribute blends as a quaternion")
{
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of_rotation("frame", tg::vec4f(0, 0, 0, 1)));
    auto const type
        = sv::material_type::create("framed", cc::move(signature), "    surface.geometry_tangent_frame = frame;");

    // Nothing supplies it: the declaration's default is a constant, and a constant has no corners to blend.
    auto const bare = sv::generate_material_shader(sv::resolve_material(type, bare_material(), make_mesh()));
    CHECK(!bare.source.contains("sv::interpolate_rotation"));
    CHECK(bare.source.contains("#define SV_ATTR_SUPPLIED_frame 0"));

    // Supplied per vertex, it interpolates as a rotation rather than through sv::interpolate_f4 — which would blend the two
    // hemispheres of the same rotation against each other.
    auto mesh = make_mesh();
    auto const frames = cc::array<tg::vec4f>{tg::vec4f(0, 0, 0, 1), tg::vec4f(0, 0, 0, 1), tg::vec4f(0, 0, 0, 1)};
    mesh.attributes.push_back(sv::mesh_attribute::create("frame", sv::attribute_frequency::per_vertex, frames));

    auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), mesh));
    CHECK(g.source.contains("sv::interpolate_rotation("));
    CHECK(!g.source.contains("sv::interpolate_f4("));
    CHECK(g.source.contains("#define SV_ATTR_SUPPLIED_frame 1"));

    // A flat frequency reads one element, so there is nothing to blend and the mode changes no code.
    auto flat = make_mesh();
    auto const one = cc::array<tg::vec4f>{tg::vec4f(0, 0, 0, 1)};
    flat.attributes.push_back(sv::mesh_attribute::create("frame", sv::attribute_frequency::per_triangle, one));

    auto const f = sv::generate_material_shader(sv::resolve_material(type, bare_material(), flat));
    CHECK(!f.source.contains("sv::interpolate_rotation"));
    CHECK(f.source.contains("sv::load_element_f4("));
}

TEST("sv::generate_material_shader - the builtin pbr type generates")
{
    auto lib = sv::material_library::create();
    sv::register_builtin_material_types(lib);
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();
    auto const gold = lib.acquire(sv::material::create("gold", pbr, {}));

    auto const g = sv::generate_material_shader(sv::resolve_material(lib, gold, make_mesh()));

    // Every attribute the type declares gets a local, whether or not anything supplied a value for it.
    for (auto const& d : lib.get_type(pbr).signature)
        CHECK(g.source.contains(cc::format("{} {} =", sv::hlsl_type_of(d.format), d.name)));
    CHECK(g.layout.slots.size() == lib.get_type(pbr).signature.size());
}
