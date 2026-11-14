#include <clean-core/container/array.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/streams/file_stream.hh>
#include <nexus/source_root.hh>
#include <nexus/test.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/resolve.hh>
#include <shaped-viewer/material/shader_generator.hh>
#include <shaped-viewer/scene/resident_mesh.hh>
#include <typed-geometry/linalg/vec.hh>

using namespace cc::primitive_defines;

// CPU-only tests for the material shader generator.
// No GPU and no compiler: what is checked here is the SGL TEXT and the parameter layout, which is what decides whether two materials share a permutation.
// That a generated hit group actually compiles is a separate, DXC-gated test.

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

/// A type with one f32 and one vec3f attribute, so both the scalar and the vector paths are covered.
[[nodiscard]] sv::material_type make_type()
{
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("roughness", 0.5f));
    signature.push_back(sv::material_signature_entry::of("base_color", tg::vec3f(0.8f, 0.8f, 0.8f)));
    return sv::material_type::create({.name = "test",
                                      .signature = cc::move(signature),
                                      .shader = "surface.specular_roughness = roughness\nsurface.base_color = "
                                                "base_color\n"});
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

TEST("sv::sgl_type_of - the supported formats and the ones that are not")
{
    CHECK(sv::sgl_type_of(sv::attribute_format_of<f32>) == "float");
    CHECK(sv::sgl_type_of(sv::attribute_format_of<tg::vec2f>) == "float2");
    CHECK(sv::sgl_type_of(sv::attribute_format_of<tg::vec3f>) == "float3");
    CHECK(sv::sgl_type_of(sv::attribute_format_of<tg::vec4f>) == "float4");
    CHECK(sv::sgl_type_of(sv::attribute_format_of<i32>) == "int");
    CHECK(sv::sgl_type_of(sv::attribute_format_of<u32>) == "uint");

    // A matrix and the narrow / wide scalars have no settled raw-buffer layout here yet, and say so by mapping to nothing.
    CHECK(sv::sgl_type_of(sv::attribute_format::of_matrix(sv::scalar_type::f32, 4, 4)).empty());
    CHECK(sv::sgl_type_of(sv::attribute_format_of<f64>).empty());
    CHECK(sv::sgl_type_of(sv::attribute_format_of<u8>).empty());
}

TEST("sv::generate_material_shader - constants come out of the parameter block")
{
    auto const type = make_type();
    auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), make_mesh()));

    CHECK(g.source.contains("use material\nuse openpbr\nuse slug\nuse tracer\n"));
    CHECK(g.source.contains("fun sv_evaluate_material(ctx: material.shading_context){tracer.bindless} -> "
                            "openpbr.surface:"));

    // One load per signature attribute, at the type the format names, read out of the parameter block.
    CHECK(g.source.contains("fun sv_attribute_roughness(ctx: material.shading_context){tracer.bindless} -> float:"));
    CHECK(g.source.contains("fun sv_attribute_base_color(ctx: material.shading_context){tracer.bindless} -> float3:"));
    CHECK(g.source.contains("return reinterpret_as_float(tracer.bindless.buffers[nonuniform "
                            "block].load(ctx.param_offset + 0u))"));
    CHECK(g.source.contains("return reinterpret_as_float(tracer.bindless.buffers[nonuniform "
                            "block].load3(ctx.param_offset + 4u))"));

    // Each attribute is an immutable local of its own name, bound before the fragment, which runs indented in the body.
    CHECK(g.source.contains("    let roughness = sv_attribute_roughness(ctx)\n"));
    CHECK(g.source.contains("    surface.specular_roughness = roughness\n"));
    CHECK(g.source.find("let base_color =") < g.source.find("surface.base_color = base_color"));

    // Nothing supplied either attribute, so the fragment's defaults are what came through — and the shading can tell.
    CHECK(g.source.contains("const sv_supplied_roughness = false"));
    CHECK(g.source.contains("const sv_supplied_base_color = false"));

    // No texture, so no palette sampler is named, and nothing cuts out, so there is no any hit.
    CHECK(!g.source.contains("palette_"));
    CHECK(g.source.contains("hit_group sv_material for tracer.path_rays:"));
    CHECK(!g.source.contains("any_hit"));

    // The layout the CPU fills is the one the source reads.
    CHECK(g.layout.slots.size() == 2);
    CHECK(slot_named(g.layout, "roughness").kind == sv::material_slot_kind::constant);
    CHECK(slot_named(g.layout, "roughness").offset == 0);
    CHECK(slot_named(g.layout, "base_color").offset == 4);
    CHECK(slot_named(g.layout, "base_color").size_bytes == 12);
    CHECK(g.layout.size_bytes == 16);

    // The key covers the resolution's shape AND the geometry it is spelled for, so it is deliberately not the permutation key.
    auto const resolved = sv::resolve_material(type, bare_material(), make_mesh());
    CHECK(g.key != resolved.permutation_key);
    CHECK(g.key == sv::material_shader_key(resolved.permutation_key, {}));
    CHECK(sv::material_shader_key(resolved.permutation_key, {.kind = sv::geometry_kind::quadrics}) != g.key);
}

TEST("sv::generate_material_shader - an attribute-less type reads no parameter block")
{
    auto const type
        = sv::material_type::create({.name = "bare", .shader = "surface.base_color = float3(1.0, 0.0, 1.0)\n"});
    auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), make_mesh()));

    CHECK(g.layout.slots.empty());
    CHECK(!g.source.contains("sv_attribute_"));
    CHECK(g.source.contains("    surface.base_color = float3(1.0, 0.0, 1.0)\n"));
    CHECK(g.source.contains("hit_group sv_material for tracer.path_rays:"));
}

// A display material reflects nothing in the tracers, so a hybrid's bounce meeting one must read it black, not as the
// mid-gray a type naming no base color is read as; the tracers absorb a bounce there, and the two must agree.
TEST("sv::generate_material_shader - a display type's secondary hit reads black, a declared base color its attribute")
{
    auto const display
        = sv::material_type::create({.name = "display",
                                     .signature = {sv::material_signature_entry::of("color", tg::vec3f(1, 1, 1))},
                                     .shader = "surface.base_weight = 0.0\n",
                                     .display_only = true});
    auto const shown = sv::generate_material_shader(sv::resolve_material(display, bare_material(), make_mesh()));
    CHECK(shown.secondary.base_color.kind == sv::material_value_kind::zero);
    CHECK(shown.secondary.specular_weight.kind == sv::material_value_kind::zero);

    auto const plain
        = sv::material_type::create({.name = "plain",
                                     .signature = {sv::material_signature_entry::of("tint", tg::vec3f(1, 1, 1))},
                                     .shader = "surface.base_color = tint\n",
                                     .base_color_attribute = "tint"});
    auto const lit = sv::generate_material_shader(sv::resolve_material(plain, bare_material(), make_mesh()));
    CHECK(lit.secondary.base_color.kind == sv::material_value_kind::constant);
    CHECK(lit.secondary.specular_weight.kind == sv::material_value_kind::none);
}

TEST("sv::generate_material_shader - a mesh attribute is loaded through its descriptor")
{
    auto const type = make_type();

    auto per_vertex = make_mesh();
    auto const values = cc::array<f32>{0.1f, 0.2f, 0.3f};
    per_vertex.attributes.push_back(
        bind(sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_vertex, values)));
    {
        auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), per_vertex));
        CHECK(g.source.contains("let desc = material.load_attribute_desc(tracer.bindless.buffers[nonuniform block], "
                                "ctx.param_offset + 0u)"));
        CHECK(g.source.contains("return material.interpolate_f1(tracer.bindless.buffers[nonuniform desc_buffer], desc, "
                                "ctx.corner, ctx.barycentrics)"));
        CHECK(g.source.contains("const sv_supplied_roughness = true"));
        CHECK(slot_named(g.layout, "roughness").kind == sv::material_slot_kind::attribute_descriptor);
        CHECK(slot_named(g.layout, "roughness").size_bytes == 12);
    }

    // per_corner reads the same way but numbers its elements off the primitive rather than off the vertex indices.
    auto per_corner = make_mesh();
    per_corner.attributes.push_back(
        bind(sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_corner, values)));
    {
        auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), per_corner));
        CHECK(g.source.contains("material.interpolate_f1"));
        CHECK(g.source.contains("material.corner_elements(ctx)"));
    }

    // per_triangle is flat — one element for the whole primitive, so it loads rather than interpolates.
    auto per_triangle = make_mesh();
    per_triangle.attributes.push_back(
        bind(sv::mesh_attribute::create("roughness", sv::attribute_frequency::per_triangle, cc::array<f32>{0.4f})));
    {
        auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), per_triangle));
        CHECK(g.source.contains("material.load_element_f1"));
        CHECK(!g.source.contains("material.interpolate_f1"));
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

    // A texture reaches the palette, which sits in the bindless group beside the tables, so a load that samples lists nothing more.
    CHECK(g.source.contains("fun sv_attribute_base_color(ctx: material.shading_context){tracer.bindless} -> float3:"));
    CHECK(g.source.contains("let uv = material.interpolate_f2("));
    CHECK(g.source.contains("let image = tracer.bindless.buffers[nonuniform block].load(ctx.param_offset + "));
    // Explicit gradients, because a ray tracing hit shader has no derivatives of its own: the ray cone's footprint axes,
    // through the triangle's uv edges.
    CHECK(g.source.contains("let grads = material.texture_gradients(ctx, material.uv_edges(tracer.bindless.buffers["
                            "nonuniform uv_desc_buffer], uv_desc, ctx.corner))"));
    CHECK(g.source.contains("let texel = tracer.bindless.textures_2d[nonuniform image].sample(uv, "
                            "tracer.bindless.palette_linear_repeat_repeat, grad_x = grads.xy, grad_y = grads.zw)"));
    // An identity swizzle narrows to the declaration's three components and stays a letter swizzle.
    CHECK(g.source.contains("return texel.xyz\n"));

    // A sampled attribute takes two slots: the texture index, and the uv descriptor it is sampled through.
    CHECK(slot_named(g.layout, "base_color").kind == sv::material_slot_kind::texture_index);
    CHECK(slot_named(g.layout, "base_color.uv").kind == sv::material_slot_kind::attribute_descriptor);
    CHECK(slot_named(g.layout, "base_color.uv").format == sv::attribute_format::of_vector(sv::scalar_type::f32, 2));
}

TEST("sv::generate_material_shader - a fragment resamples an attribute at a uv of its own")
{
    auto const type = make_type(); // roughness (f32) and base_color (vec3f)

    auto mesh = make_mesh();
    mesh.attributes.push_back(make_uvs());
    mesh.textures.push_back(
        {.name = "base_color",
         .source
         = {.texture = sv::texture_id(3), .uv_attribute = "uv", .transform = sv::sample_transform::of_signed_normal()}});

    auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), mesh));

    // A sampled attribute samples the same texture through the same transform, at the uv it is handed rather than the mesh's.
    CHECK(g.source.contains("fun sv_base_color_at(ctx: material.shading_context, at: float2){tracer.bindless} -> "
                            "float3:\n"));
    CHECK(g.source.contains("    let uv = at\n"));
    CHECK(g.source.contains("return texel.xyz * scale.xyz + bias.xyz\n"));

    // An attribute with no uv to move returns its value, so a fragment resampling it compiles whatever it resolved at.
    CHECK(g.source.contains("fun sv_roughness_at(ctx: material.shading_context, at: float2){tracer.bindless} -> "
                            "float:\n"
                            "    return sv_attribute_roughness(ctx)\n"));

    // The raster pixel stage lets its derivatives pick the level, here as in the attribute's own load.
    CHECK(g.raster_source.contains("fun sv_base_color_at("));
    CHECK(!g.raster_source.contains("level = 0.0"));
}

TEST("sv::generate_material_shader - a channel swizzle picks what each component reads")
{
    auto const type = make_type(); // roughness (f32) and base_color (vec3f), so both widths are covered

    // The packing this exists for: one metallic-roughness map, whose green channel is the roughness.
    auto packed = make_mesh();
    packed.attributes.push_back(make_uvs());
    packed.textures.push_back({.name = "roughness",
                               .source = {.texture = sv::texture_id(3),
                                          .uv_attribute = "uv",
                                          .swizzle = sv::channel_swizzle::of_channel(sv::texture_channel::g)}});

    auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), packed));
    CHECK(g.source.contains("return texel.y\n"));

    // A constant selector is not a channel, so it cannot be spelled as a letter swizzle and widens through a constructor instead.
    auto with_constant = make_mesh();
    with_constant.attributes.push_back(make_uvs());
    with_constant.textures.push_back(
        {.name = "base_color",
         .source = {.texture = sv::texture_id(3),
                    .uv_attribute = "uv",
                    .swizzle = sv::channel_swizzle::of(sv::texture_channel::b, sv::texture_channel::g,
                                                       sv::texture_channel::one)}});

    auto const c = sv::generate_material_shader(sv::resolve_material(type, bare_material(), with_constant));
    CHECK(c.source.contains("return float3(texel.z, texel.y, 1.0)\n"));
}

TEST("sv::generate_material_shader - a sample transform is a multiply-add over parameters")
{
    auto const type = make_type(); // roughness (f32) and base_color (vec3f)

    auto const with = [&](cc::string name, sv::sample_transform transform)
    {
        auto mesh = make_mesh();
        mesh.attributes.push_back(make_uvs());
        mesh.textures.push_back({.name = cc::move(name),
                                 .source = {.texture = sv::texture_id(3), .uv_attribute = "uv", .transform = transform}});
        return sv::generate_material_shader(sv::resolve_material(type, bare_material(), mesh));
    };

    // An identity transform costs neither a slot nor an instruction — which is what makes it free to have this field.
    auto const plain = with("base_color", {});
    CHECK(!plain.source.contains("let scale"));
    CHECK(plain.layout.slots.size() == 3); // roughness' constant, plus base_color's texture index and uv descriptor

    // A normal map's decode reads its numbers out of the block rather than out of the source.
    auto const decoded = with("base_color", sv::sample_transform::of_signed_normal());
    CHECK(decoded.source.contains("let scale = reinterpret_as_float(tracer.bindless.buffers[nonuniform block].load4("));
    CHECK(decoded.source.contains("return texel.xyz * scale.xyz + bias.xyz\n"));
    CHECK(slot_named(decoded.layout, "base_color.transform").kind == sv::material_slot_kind::sample_transform);
    CHECK(slot_named(decoded.layout, "base_color.transform").size_bytes == 32);

    // A scalar attribute reads one lane of each.
    auto const scalar = with("roughness", sv::sample_transform::of_strength(0.5f));
    CHECK(scalar.source.contains("return texel.x * scale.x + bias.x\n"));
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

    // Two materials differing only in their constants generate BYTE-IDENTICAL source.
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

TEST("sv::generate_material_shader - two attributes sampled alike sample through one palette sampler")
{
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("base_color", tg::vec3f(0.8f, 0.8f, 0.8f)));
    signature.push_back(sv::material_signature_entry::of("emissive", tg::vec3f(0.0f, 0.0f, 0.0f)));
    auto const type = sv::material_type::create(
        {.name = "two", .signature = cc::move(signature), .shader = "surface.base_color = base_color\n"});

    auto mesh = make_mesh();
    mesh.attributes.push_back(make_uvs());
    mesh.textures.push_back({.name = "base_color", .source = {.texture = sv::texture_id(1), .uv_attribute = "uv"}});
    mesh.textures.push_back({.name = "emissive", .source = {.texture = sv::texture_id(2), .uv_attribute = "uv"}});

    auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), mesh));
    auto const sampler = cc::string_view("tracer.bindless.palette_linear_repeat_repeat");
    auto const first = g.source.find(sampler);
    REQUIRE(first >= 0);
    CHECK(cc::string_view(g.source).subview(first + sampler.size()).contains(sampler));
    CHECK(!g.source.contains("palette_nearest"));

    // Two textures, two uv descriptors — the texture id is a parameter, so the two ids do not multiply the permutations.
    CHECK(g.layout.slots.size() == 4);
}

TEST("sv::generate_material_shader - a rotation attribute blends as a quaternion")
{
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of_rotation("frame", tg::vec4f(0, 0, 0, 1)));
    auto const type = sv::material_type::create(
        {.name = "framed", .signature = cc::move(signature), .shader = "surface.geometry_tangent_frame = frame\n"});

    // Nothing supplies it: the declaration's default is a constant, and a constant has no corners to blend.
    auto const bare = sv::generate_material_shader(sv::resolve_material(type, bare_material(), make_mesh()));
    CHECK(!bare.source.contains("material.interpolate_rotation"));
    CHECK(bare.source.contains("const sv_supplied_frame = false"));

    // Supplied per vertex, it interpolates as a rotation rather than through `material.interpolate_f4`.
    // That would blend the two hemispheres of the same rotation against each other.
    auto mesh = make_mesh();
    auto const frames = cc::array<tg::vec4f>{tg::vec4f(0, 0, 0, 1), tg::vec4f(0, 0, 0, 1), tg::vec4f(0, 0, 0, 1)};
    mesh.attributes.push_back(bind(sv::mesh_attribute::create("frame", sv::attribute_frequency::per_vertex, frames)));

    auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), mesh));
    CHECK(g.source.contains("material.interpolate_rotation("));
    CHECK(!g.source.contains("material.interpolate_f4("));
    CHECK(g.source.contains("const sv_supplied_frame = true"));

    // A flat frequency reads one element, so there is nothing to blend and the mode changes no code.
    auto flat = make_mesh();
    auto const one = cc::array<tg::vec4f>{tg::vec4f(0, 0, 0, 1)};
    flat.attributes.push_back(bind(sv::mesh_attribute::create("frame", sv::attribute_frequency::per_triangle, one)));

    auto const f = sv::generate_material_shader(sv::resolve_material(type, bare_material(), flat));
    CHECK(!f.source.contains("material.interpolate_rotation"));
    CHECK(f.source.contains("material.load_element_f4("));
}

TEST("sv::generate_material_shader - the raster pixel stage evaluates the same material body")
{
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("base_color", tg::vec3f(0.8f, 0.8f, 0.8f)));
    signature.push_back(sv::material_signature_entry::of("opacity", 1.0f));
    signature.push_back(sv::material_signature_entry::of("cutoff", 0.0f));
    auto const type = sv::material_type::create({.name = "cut",
                                                 .signature = cc::move(signature),
                                                 .shader = "surface.base_color = base_color\n",
                                                 .opacity_attribute = "opacity",
                                                 .opacity_cutoff_attribute = "cutoff"});

    auto mesh = make_mesh();
    mesh.attributes.push_back(make_uvs());
    mesh.textures.push_back({.name = "base_color", .source = {.texture = sv::texture_id(3), .uv_attribute = "uv"}});
    mesh.attributes.push_back(
        bind(sv::mesh_attribute::create("opacity", sv::attribute_frequency::per_triangle, cc::array<f32>{0.3f})));
    auto const material
        = sv::material::create("m", sv::material_type_id(0), {sv::material_attribute_binding::of("cutoff", 0.5f)});
    auto const g = sv::generate_material_shader(sv::resolve_material(type, material, mesh));
    REQUIRE(g.opacity.can_cut_out());

    // The fragment and every load come through as the hit group has them, through the same palette: only the level differs.
    CHECK(g.raster_source.contains("use raster\n"));
    CHECK(g.raster_source.contains("    surface.base_color = base_color\n"));
    CHECK(g.raster_source.contains("fun sv_attribute_base_color(ctx: material.shading_context){tracer.bindless} -> "
                                   "float3:"));
    // A pixel has derivatives, so it samples at the level they pick rather than at level 0.
    CHECK(g.raster_source.contains(".sample(uv, tracer.bindless.palette_linear_repeat_repeat)\n"));
    CHECK(!g.raster_source.contains("tracer.traced"));
    CHECK(!g.raster_source.contains("level = 0.0"));
    CHECK(!g.raster_source.contains("require raytracing_pipeline"));

    // The cut reads the instance's opacity record through the any-hit's reader, ahead of the evaluation, rather than the material's loads.
    // The draw is indexed, so the pixel takes its triangle from `@primitive_id`, which the device must have.
    CHECK(g.raster_source.contains("require primitive_index\n"));
    CHECK(g.raster_source.contains("@pixel fun sv_raster(p: raster.varyings, @primitive_id primitive: "
                                   "int){raster.drawn, "
                                   "tracer.bindless, slug.tables} -> raster.targets:"));
    CHECK(g.raster_source.contains("    if raster.is_cut(p, ctx) => discard\n"));
    CHECK(g.raster_source.find("raster.is_cut") < g.raster_source.find("raster.shade("));

    // The hit group samples the same palette, with the ray cone's gradients.
    CHECK(g.source.contains("tracer.bindless.palette_linear_repeat_repeat, grad_x = grads.xy, grad_y = grads.zw"));

    // A material that cannot cut out has no cut to make.
    auto const opaque = sv::generate_material_shader(sv::resolve_material(type, bare_material(), make_mesh()));
    REQUIRE(!opaque.opacity.can_cut_out());
    CHECK(!opaque.raster_source.contains("raster.is_cut"));

    // A quadric batch's stages solve the surface along the pixel's ray over its box, and write its depth.
    auto const quadric = sv::generate_material_shader(sv::resolve_material(type, material, make_mesh()),
                                                      {.kind = sv::geometry_kind::quadrics});
    CHECK(quadric.raster_source.contains("raster.quadric_pixel_of(p)"));
    CHECK(quadric.raster_source.contains("raster.quadric_targets"));
    CHECK(quadric.raster_source.contains("raster.quadric_thin_targets"));
    CHECK(quadric.hybrid_source.contains("hybrid.quadric_pixel_of(p)"));
}

TEST("sv::generate_material_shader - the raster pixel stage hands shade the type's occlusion, or 1")
{
    auto signature = cc::vector<sv::material_signature_entry>();
    signature.push_back(sv::material_signature_entry::of("base_color", tg::vec3f(0.8f, 0.8f, 0.8f)));
    signature.push_back(sv::material_signature_entry::of("ao", 1.0f));
    auto const type = sv::material_type::create({.name = "occluded",
                                                 .signature = cc::move(signature),
                                                 .shader = "surface.base_color = base_color\n",
                                                 .occlusion_attribute = "ao"});
    auto const g = sv::generate_material_shader(sv::resolve_material(type, bare_material(), make_mesh()));
    CHECK(g.raster_source.contains("    return raster.shade(p, ctx, sv_evaluate_material(ctx), false, "
                                   "sv_attribute_ao(ctx))\n"));

    auto const plain_type = make_type();
    auto const plain = sv::generate_material_shader(sv::resolve_material(plain_type, bare_material(), make_mesh()));
    CHECK(plain.raster_source.contains("    return raster.shade(p, ctx, sv_evaluate_material(ctx), false, 1.0)\n"));
}

TEST("sv::generate_material_shader - the builtin pbr type generates")
{
    auto lib = sv::material_library::create();
    sv::register_builtin_material_types(lib);
    auto const pbr = lib.acquire_type(sv::builtin_material::pbr).value();
    auto const gold = lib.acquire(sv::material::create("gold", pbr, {}));

    auto const g = sv::generate_material_shader(sv::resolve_material(lib, gold, make_mesh()));

    // Every attribute the type declares gets a load and a local, whether or not anything supplied a value for it.
    for (auto const& d : lib.get_type(pbr).signature)
    {
        CHECK(g.source.contains(cc::format("-> {}:", sv::sgl_type_of(d.format))));
        CHECK(g.source.contains(cc::format("    let {} = sv_attribute_{}(ctx)", d.name, d.name)));
    }
    CHECK(g.layout.slots.size() == lib.get_type(pbr).signature.size());
}

TEST("sv - the committed pbr shade kernel is what the generator writes, which the native size gate measures")
{
    // native-parity.py's `check` holds SGL's binaries of this file to a baseline, and it stands for every material's
    // shade kernel, which the program generates at run time; SC_UPDATE_NATIVE_SAMPLES=1 writes it anew
    auto materials = sv::material_library::create();
    sv::register_builtin_material_types(materials);
    auto const type = materials.acquire_type(sv::builtin_material::pbr).value();
    auto const id = materials.acquire(sv::material::create("pbr", type, {}));
    auto const generated = sv::generate_material_shader(sv::resolve_material(materials, id, make_mesh())).shade_source;
    auto const path = nx::source_path(SV_TEST_DATA_DIR "/pbr_shade_kernel.sgl");

    if (cc::environment_variable("SC_UPDATE_NATIVE_SAMPLES").has_value())
    {
        auto file = cc::file_write_stream_adapter::create(path);
        REQUIRE(file.has_value());
        auto stream = file.value().stream();
        REQUIRE(stream.write(generated.as_bytes()).has_value());
        REQUIRE(stream.flush().has_value());
    }
    auto file = cc::file_read_stream_adapter::open(path);
    REQUIRE(file.has_value());
    auto stream = file.value().stream();
    auto const bytes = stream.read_all();
    REQUIRE(bytes.has_value());
    auto const committed = cc::string_view(reinterpret_cast<char const*>(bytes.value().data()), bytes.value().size());
    CHECK(committed == cc::string_view(generated))
        .dump("fix", "run `SC_UPDATE_NATIVE_SAMPLES=1 uv run dev.py test \"the committed pbr shade kernel\"`, then "
                     "`native-parity.py update` where the gate moves");
}
