#include "shader_generator.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <shaped-viewer/material/impl/material_hash.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>
#include <shaped-viewer/scene/resident_mesh.hh>

namespace sv
{
namespace
{
constexpr i32 slot_alignment = 4;         ///< raw buffer loads are 4-byte granular, so every slot starts on 4
constexpr i32 attribute_desc_size = 12;   ///< sv::attribute_desc: buffer, offset, stride
constexpr i32 sample_transform_size = 32; ///< two float4s: the scale, then the bias

[[nodiscard]] i32 align_up(i32 value, i32 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

/// The `material.interpolate_*` / `material.load_element_*` suffix for a component count.
[[nodiscard]] cc::string_view load_suffix(int components)
{
    switch (components)
    {
    case 1:
        return "f1";
    case 2:
        return "f2";
    case 3:
        return "f3";
    case 4:
        return "f4";
    default:
        return "";
    }
}

/// The `xyzw` prefix that narrows a `float4` to `components` — what a scale or a bias is read through.
[[nodiscard]] cc::string_view component_swizzle(int components)
{
    switch (components)
    {
    case 1:
        return "x";
    case 2:
        return "xy";
    case 3:
        return "xyz";
    default:
        return "xyzw";
    }
}

/// How a frequency is read: one element, or three blended across a triangle.
enum class load_shape
{
    flat,        ///< one element, indexed directly
    barycentric, ///< three corners weighted by `ctx.barycentrics`
};

[[nodiscard]] load_shape shape_of(attribute_frequency f)
{
    switch (f)
    {
    case attribute_frequency::per_vertex:
    case attribute_frequency::per_corner:
        return load_shape::barycentric;
    default:
        // `per_triangle` is one element for the whole primitive, whichever geometry numbered it.
        return load_shape::flat;
    }
}

/// The parameter block: one slot per thing the shader has to be told at run time, in signature order.
[[nodiscard]] material_parameter_layout build_layout(resolved_material const& r)
{
    auto layout = material_parameter_layout();
    auto offset = 0;

    auto const push = [&](cc::string name, material_slot_kind kind, i32 size, attribute_format format, i32 index)
    {
        offset = align_up(offset, slot_alignment);
        layout.slots.push_back({.name = cc::move(name),
                                .kind = kind,
                                .offset = offset,
                                .size_bytes = size,
                                .format = format,
                                .attribute_index = index});
        offset += size;
    };

    for (auto i = 0; i < r.attributes.size(); ++i)
    {
        auto const& a = r.attributes[i];
        switch (a.frequency)
        {
        case material_frequency::material_type:
        case material_frequency::material:
        case material_frequency::mesh_instance:
            push(cc::string(a.name), material_slot_kind::constant, a.format.size_bytes(), a.format, i32(i));
            break;

        case material_frequency::mesh_attribute:
            push(cc::string(a.name), material_slot_kind::attribute_descriptor, attribute_desc_size, a.format, i32(i));
            break;

        case material_frequency::material_texture:
        case material_frequency::mesh_texture_binding:
            push(cc::string(a.name), material_slot_kind::texture_index, i32(sizeof(u32)), a.format, i32(i));
            push(cc::format("{}.uv", a.name), material_slot_kind::attribute_descriptor, attribute_desc_size,
                 attribute_format::of_vector(scalar_type::f32, 2), i32(i));

            // Only when there is something to apply: an identity transform is the overwhelming majority, and it must
            // cost neither a slot nor an instruction.
            if (!a.sample->transform.is_identity(a.format.component_count()))
                push(cc::format("{}.transform", a.name), material_slot_kind::sample_transform, sample_transform_size,
                     attribute_format::of_vector(scalar_type::f32, 4), i32(i));
            break;
        }
    }

    layout.size_bytes = align_up(offset, slot_alignment);
    return layout;
}

/// The slot serving `index` of the given kind, which `build_layout` guarantees exists.
[[nodiscard]] material_slot const& slot_for(material_parameter_layout const& layout, i32 index, material_slot_kind kind)
{
    for (auto const& s : layout.slots)
        if (s.attribute_index == index && s.kind == kind)
            return s;
    CC_UNREACHABLE("the layout has a slot for every attribute it was built from");
}

/// The `xyzw` letter one selector names, or empty for a constant one: SGL's swizzle letters are the field names.
[[nodiscard]] cc::string_view channel_letter(texture_channel c)
{
    switch (c)
    {
    case texture_channel::r:
        return "x";
    case texture_channel::g:
        return "y";
    case texture_channel::b:
        return "z";
    case texture_channel::a:
        return "w";
    case texture_channel::zero:
    case texture_channel::one:
        return {};
    }
    return {};
}

/// The float vector type of `components` lanes.
[[nodiscard]] cc::string_view float_type(int components)
{
    switch (components)
    {
    case 1:
        return "float";
    case 2:
        return "float2";
    case 3:
        return "float3";
    default:
        return "float4";
    }
}

/// `value`, a float expression of `format`'s lane count, converted to `format` where that is an integer type.
[[nodiscard]] cc::string as_format(cc::string value, attribute_format format)
{
    if (format.scalar == scalar_type::f32)
        return value;
    return cc::format("({}) as {}", value, sgl_type_of(format));
}

/// The float expression filling an attribute of `format` from the `float4` named `texel`.
///
/// A swizzle whose every read selector names a channel stays a letter swizzle — `texel.xyz`, or `texel` itself when all four
/// are taken straight through — which is what a texture written for its own attribute generates.
/// A `zero` or `one` selector cannot be spelled that way, so those widen through a constructor instead.
/// Either form narrows or widens to `component_count()`, so a 4-channel texture serving a scalar attribute reads one channel.
[[nodiscard]] cc::string texel_expression(channel_swizzle const& z, attribute_format format)
{
    auto const components = format.component_count();

    auto letters = cc::string();
    for (auto i = 0; i < components; ++i)
    {
        auto const letter = channel_letter(z.components[i]);
        if (letter.empty())
        {
            letters.clear();
            break;
        }
        letters += letter;
    }

    if (!letters.empty())
        return components == 4 && z.is_identity(4) ? cc::string("texel") : cc::format("texel.{}", letters);

    auto const component = [&](texture_channel c) -> cc::string
    {
        if (c == texture_channel::zero)
            return "0.0";
        if (c == texture_channel::one)
            return "1.0";
        return cc::format("texel.{}", channel_letter(c));
    };
    if (components == 1)
        return component(z.components[0]);

    auto args = cc::string();
    for (auto i = 0; i < components; ++i)
    {
        if (i > 0)
            args += ", ";
        args += component(z.components[i]);
    }
    return cc::format("{}({})", float_type(components), args);
}

/// The element index (or indices) a frequency reads.
///
/// `per_triangle` is `ctx.primitive` whichever geometry numbered it, which is what makes one generated body serve a mesh and a
/// quadric batch alike.
[[nodiscard]] cc::string_view element_expression(attribute_frequency f)
{
    switch (f)
    {
    case attribute_frequency::per_vertex:
        return "ctx.corner";
    case attribute_frequency::per_corner:
        return "material.corner_elements(ctx)";
    case attribute_frequency::per_triangle:
        return "ctx.primitive";
    default:
        CC_UNREACHABLE("a mesh attribute a material reads is per_vertex, per_corner or per_triangle");
    }
}

/// The load of mesh attribute `binding` through the descriptor named `desc`, as a float expression of `components` lanes.
/// It reads the buffer through a local `<desc>_buffer`, since only a name may stand as a non-uniform index.
[[nodiscard]] cc::string mesh_load(mesh_attribute_binding const& binding, cc::string_view desc, int components, bool rotates)
{
    auto const buffer = cc::format("tracer.bindless.buffers[nonuniform {}_buffer]", desc);
    if (shape_of(binding.frequency) == load_shape::flat)
        return cc::format("material.load_element_{}({}, {}, {})", load_suffix(components), buffer, desc,
                          element_expression(binding.frequency));
    // A rotation blends as one: the three corners are aligned into a common hemisphere before they are summed.
    auto const blend = rotates ? cc::string("rotation") : cc::string(load_suffix(components));
    return cc::format("material.interpolate_{}({}, {}, {}, ctx.barycentrics)", blend, buffer, desc,
                      element_expression(binding.frequency));
}

/// What differs between the stages a material's body is spelled for: how a texture picks its level.
///
/// Everything else — the loads, the fragment, the bindless tables and the sampler palette among them — is the same text for a traced hit and a rasterized pixel.
struct body_spelling
{
    /// Whether every sample names level 0, which a hit has to since it has no derivatives, or lets the pixel's derivatives pick it.
    /// A pixel's cutout does not read through these loads: `raster.is_cut` reads `tracer.instance_opacity`, at level 0 as a hit does.
    bool explicit_level = true;
};

/// The spelling a hit group and a wavefront shade kernel read their attributes in.
constexpr auto traced_spelling = body_spelling{};

/// The spelling a raster pixel stage reads its attributes in.
constexpr auto raster_spelling = body_spelling{.explicit_level = false};

/// `fun sv_attribute_<name>(ctx)`: the load of attribute `index`, out of the parameter block, a mesh attribute or a texture.
[[nodiscard]] cc::string attribute_function(resolved_material const& r,
                                            material_parameter_layout const& layout,
                                            i32 index,
                                            body_spelling const& spelling)
{
    auto const& a = r.attributes[index];
    auto const components = a.format.component_count();

    auto out = cc::string();
    cc::format_append(out, "fun sv_attribute_{}(ctx: material.shading_context){{tracer.bindless}} -> {}:\n", a.name,
                      sgl_type_of(a.format));
    out += "    let block = ctx.param_buffer as int\n";

    switch (a.frequency)
    {
    case material_frequency::material_type:
    case material_frequency::material:
    case material_frequency::mesh_instance:
    {
        // A constant is read out of the parameter block whatever it is worth, which is why gold and copper share this source.
        auto const& s = slot_for(layout, index, material_slot_kind::constant);
        auto const load = components == 1 ? cc::string("load") : cc::format("load{}", components);
        auto const raw
            = cc::format("tracer.bindless.buffers[nonuniform block].{}(ctx.param_offset + {}u)", load, s.offset);
        auto value = raw;
        if (a.format.scalar == scalar_type::f32)
            value = cc::format("reinterpret_as_float({})", raw);
        else if (a.format.scalar == scalar_type::i32)
            value = cc::format("reinterpret_as_int({})", raw);
        cc::format_append(out, "    return {}\n", value);
        break;
    }

    case material_frequency::mesh_attribute:
    {
        auto const& s = slot_for(layout, index, material_slot_kind::attribute_descriptor);
        cc::format_append(out,
                          "    let desc = material.load_attribute_desc(tracer.bindless.buffers[nonuniform block], "
                          "ctx.param_offset + {}u)\n",
                          s.offset);
        out += "    let desc_buffer = desc.buffer as int\n";
        auto const rotates = a.interpolation == attribute_interpolation::rotation;
        cc::format_append(out, "    return {}\n",
                          as_format(mesh_load(*a.attribute, "desc", components, rotates), a.format));
        break;
    }

    case material_frequency::material_texture:
    case material_frequency::mesh_texture_binding:
    {
        auto const& tex = slot_for(layout, index, material_slot_kind::texture_index);
        auto const& uv_slot = slot_for(layout, index, material_slot_kind::attribute_descriptor);
        cc::format_append(out,
                          "    let uv_desc = material.load_attribute_desc(tracer.bindless.buffers[nonuniform block], "
                          "ctx.param_offset + {}u)\n",
                          uv_slot.offset);
        out += "    let uv_desc_buffer = uv_desc.buffer as int\n";
        // A uv is only ever a triangle attribute, so the two shapes are the barycentric one and the flat one.
        cc::format_append(out, "    let uv = {}\n", mesh_load(*a.uv, "uv_desc", 2, false));
        cc::format_append(
            out, "    let image = tracer.bindless.buffers[nonuniform block].load(ctx.param_offset + {}u) as int\n",
            tex.offset);
        // An explicit level where there are no derivatives, at a hit.
        // The texel is named because a constant selector reads it twice.
        cc::format_append(
            out, "    let texel = tracer.bindless.textures_2d[nonuniform image].sample(uv, tracer.bindless.{}{})\n",
            sgl_palette_sampler(a.sample->sampler), spelling.explicit_level ? ", level = 0.0" : "");

        auto value = texel_expression(a.sample->swizzle, a.format);

        // The scale and the bias are parameters rather than literals, so a material changing only its normal scale re-uses this source.
        if (!a.sample->transform.is_identity(components))
        {
            auto const& tf = slot_for(layout, index, material_slot_kind::sample_transform);
            auto const lanes = component_swizzle(components);
            cc::format_append(out,
                              "    let scale = reinterpret_as_float(tracer.bindless.buffers[nonuniform block].load4("
                              "ctx.param_offset + {}u))\n"
                              "    let bias = reinterpret_as_float(tracer.bindless.buffers[nonuniform block].load4("
                              "ctx.param_offset + {}u))\n",
                              tf.offset, tf.offset + 16);
            value = cc::format("{} * scale.{} + bias.{}", value, lanes, lanes);
        }
        cc::format_append(out, "    return {}\n", as_format(cc::move(value), a.format));
        break;
    }
    }
    return out;
}

/// Module `material`'s `frequency_*` for a mesh attribute's frequency.
[[nodiscard]] u32 frequency_code(attribute_frequency f)
{
    switch (f)
    {
    case attribute_frequency::per_vertex:
        return 1;
    case attribute_frequency::per_corner:
        return 2;
    default:
        return 0;
    }
}

/// `tracer.texel_channel`'s number for one swizzle selector.
[[nodiscard]] u32 channel_code(texture_channel c)
{
    switch (c)
    {
    case texture_channel::r:
        return 0;
    case texture_channel::g:
        return 1;
    case texture_channel::b:
        return 2;
    case texture_channel::a:
        return 3;
    case texture_channel::zero:
        return 4;
    case texture_channel::one:
        return 5;
    }
    return 0;
}

/// Where `r`'s cutout opacity lives in `layout`, or `none` where the permutation cannot cut out.
///
/// It cannot where the type declares no opacity attribute, or where nothing supplied it: at `material_type` frequency the
/// value is the signature's own default, and an any-hit would run at every intersection to reject nothing.
[[nodiscard]] material_opacity opacity_of(resolved_material const& r, material_parameter_layout const& layout)
{
    auto out = material_opacity();
    if (r.type->opacity_attribute.empty())
        return out;

    for (auto i = 0; i < r.attributes.size(); ++i)
    {
        auto const& a = r.attributes[i];
        if (a.name != r.type->opacity_attribute)
            continue;

        switch (a.frequency)
        {
        case material_frequency::material_type:
            // The signature's own default, which nothing supplied: it cannot cut out.
            break;

        case material_frequency::material:
        case material_frequency::mesh_instance:
            out.kind = material_opacity_kind::constant;
            out.offset = u32(slot_for(layout, i32(i), material_slot_kind::constant).offset);
            break;

        case material_frequency::mesh_attribute:
            out.kind = material_opacity_kind::attribute;
            out.offset = u32(slot_for(layout, i32(i), material_slot_kind::attribute_descriptor).offset);
            out.frequency = frequency_code(a.attribute->frequency);
            break;

        case material_frequency::material_texture:
        case material_frequency::mesh_texture_binding:
            out.kind = material_opacity_kind::texture;
            out.offset = u32(slot_for(layout, i32(i), material_slot_kind::texture_index).offset);
            out.uv_offset = u32(slot_for(layout, i32(i), material_slot_kind::attribute_descriptor).offset);
            out.frequency = frequency_code(a.uv->frequency);
            out.channel = channel_code(a.sample->swizzle.components[0]);
            out.sampler = palette_index(a.sample->sampler);
            if (!a.sample->transform.is_identity(a.format.component_count()))
                out.transform = u32(slot_for(layout, i32(i), material_slot_kind::sample_transform).offset);
            break;
        }
    }
    if (!out.can_cut_out() || r.type->opacity_cutoff_attribute.empty())
        return out;

    // A cutoff is a threshold, so it has to be one value for the instance: sourced from a texture or a mesh attribute, it is ignored.
    for (auto i = 0; i < r.attributes.size(); ++i)
    {
        auto const& a = r.attributes[i];
        if (a.name != r.type->opacity_cutoff_attribute)
            continue;
        auto const constant = a.frequency == material_frequency::material_type
                           || a.frequency == material_frequency::material
                           || a.frequency == material_frequency::mesh_instance;
        if (constant)
            out.cutoff = u32(slot_for(layout, i32(i), material_slot_kind::constant).offset);
        else
            CC_LOG_WARNING("material type '{}' resolved its opacity cutoff '{}' from a texture or a mesh attribute, "
                           "which a "
                           "threshold cannot be; the cutout falls back to coverage",
                           r.type->name, a.name);
    }
    return out;
}

/// Where attribute `name` of `r` lives in `layout`, read as one factor of the emission; `none` where `name` is empty.
///
/// Unlike the opacity, a value at `material_type` frequency counts: the signature's default is a real emission.
/// A default of zero is what keeps an instance from qualifying as a light at all.
[[nodiscard]] material_emission_factor emission_factor_of(resolved_material const& r,
                                                          material_parameter_layout const& layout,
                                                          cc::string_view name)
{
    auto out = material_emission_factor();
    if (name.empty())
        return out;

    for (auto i = 0; i < r.attributes.size(); ++i)
    {
        auto const& a = r.attributes[i];
        if (a.name != name)
            continue;

        switch (a.frequency)
        {
        case material_frequency::material_type:
        case material_frequency::material:
        case material_frequency::mesh_instance:
            out.kind = material_opacity_kind::constant;
            out.offset = u32(slot_for(layout, i32(i), material_slot_kind::constant).offset);
            break;

        case material_frequency::mesh_attribute:
            out.kind = material_opacity_kind::attribute;
            out.offset = u32(slot_for(layout, i32(i), material_slot_kind::attribute_descriptor).offset);
            out.frequency = frequency_code(a.attribute->frequency);
            break;

        case material_frequency::material_texture:
        case material_frequency::mesh_texture_binding:
            out.kind = material_opacity_kind::texture;
            out.offset = u32(slot_for(layout, i32(i), material_slot_kind::texture_index).offset);
            out.uv_offset = u32(slot_for(layout, i32(i), material_slot_kind::attribute_descriptor).offset);
            out.frequency = frequency_code(a.uv->frequency);
            for (auto c = 0; c < 3; ++c)
                out.channels[c] = channel_code(a.sample->swizzle.components[c]);
            out.sampler = palette_index(a.sample->sampler);
            if (!a.sample->transform.is_identity(a.format.component_count()))
                out.transform = u32(slot_for(layout, i32(i), material_slot_kind::sample_transform).offset);
            break;
        }
    }
    return out;
}

/// Where `r`'s emission lives in `layout`, as its type declares it.
[[nodiscard]] material_emission emission_of(resolved_material const& r, material_parameter_layout const& layout)
{
    if (!r.type->declares_emission())
        return {};
    return {.color = emission_factor_of(r, layout, r.type->emission_color_attribute),
            .scale = emission_factor_of(r, layout, r.type->emission_scale_attribute)};
}

/// The header every generated source of one permutation opens with: what it is, what it requires, and the modules it joins.
/// `extra_use` is one more module the stages join, or empty.
[[nodiscard]] cc::string source_header(resolved_material const& r,
                                       geometry_kind kind,
                                       cc::string_view requires_,
                                       cc::string_view extra_use = {})
{
    auto src = cc::string();
    cc::format_append(src, "// generated from material type '{}' — do not edit\n", r.type->name);
    src += requires_;
    src += "\nuse material\nuse openpbr\n";
    if (kind == geometry_kind::quadrics)
        src += "use quadric\n";
    if (!extra_use.empty())
        cc::format_append(src, "use {}\n", extra_use);
    src += "use slug\nuse tracer\n\n";
    return src;
}

/// The material itself, which every source of a permutation shares: the `sv_supplied_*` constants, an attribute
/// function per attribute, and `sv_evaluate_material`.
/// Hands back what a stage passes as `supplied_tangent_frame`.
[[nodiscard]] cc::string material_body(resolved_material const& r,
                                       material_parameter_layout const& layout,
                                       cc::string& supplied_frame,
                                       body_spelling const& spelling)
{
    auto src = cc::string();

    // Whether anything actually supplied each attribute, as a compile-time constant per permutation.
    //
    // A default is not a value somebody chose, and for some attributes those two must not shade the same way: an unsupplied
    // tangent frame has to fall back to the geometric one rather than to the identity rotation, which points at object-space
    // +z and is a frame belonging to no surface.
    // The resolution knows the difference and the fragment does not, so it is spelled here — for every attribute rather than
    // for the ones that happen to care, which keeps this a property of the generator rather than a list of names in it.
    for (auto const& a : r.attributes)
        cc::format_append(src, "const sv_supplied_{} = {}\n", a.name,
                          a.frequency == material_frequency::material_type ? "false" : "true");
    if (!r.attributes.empty())
        src += "\n";

    for (auto i = 0; i < r.attributes.size(); ++i)
    {
        src += attribute_function(r, layout, i32(i), spelling);
        src += "\n";
    }

    src += "fun sv_evaluate_material(ctx: material.shading_context){tracer.bindless} -> openpbr.surface:\n";
    for (auto const& a : r.attributes)
        cc::format_append(src, "    let {} = sv_attribute_{}(ctx)\n", a.name, a.name);
    src += "    let mut surface = openpbr.default_surface()\n";
    cc::format_append(src, "\n    // --- {} ---\n", r.type->name);
    auto line_start = isize(0);
    auto const& fragment = r.type->shader;
    while (line_start < fragment.size())
    {
        auto line_end = line_start;
        while (line_end < fragment.size() && fragment[line_end] != '\n')
            ++line_end;
        auto const line = cc::string_view(fragment).subview(cc::start_end{.start = line_start, .end = line_end});
        if (!line.empty())
            cc::format_append(src, "    {}", line);
        src += "\n";
        line_start = line_end + 1;
    }

    // A type declaring its emission has it written here, after the fragment, from the declared attributes alone.
    // That is the value the light sampler reads as well, so the two strategies weighed against each other see one emission.
    // A traced spelling samples a texture at level 0, the level `tracer.emission_factor` reads too; a raster pixel's level is no light strategy.
    if (r.type->declares_emission())
    {
        src += "\n    // --- the declared emission ---\n";
        cc::format_append(src, "    surface.emission_color = max(float3(0.0), {})\n", r.type->emission_color_attribute);
        if (r.type->emission_scale_attribute.empty())
            src += "    surface.emission_luminance = 1.0\n";
        else
            cc::format_append(src, "    surface.emission_luminance = max(0.0, {})\n", r.type->emission_scale_attribute);
    }
    src += "    return surface\n\n";

    // The tangent frame is the one supplied attribute the shading itself asks after, and a type that declares none supplies none.
    supplied_frame = cc::string("false");
    for (auto const& a : r.attributes)
        if (a.name == "tangent_frame")
            supplied_frame = "sv_supplied_tangent_frame";
    return src;
}

/// The hit group of one permutation; `generate_material_shader`'s header says what it holds.
[[nodiscard]] cc::string generate_hit_group(resolved_material const& r,
                                            material_parameter_layout const& layout,
                                            geometry_kind kind,
                                            bool can_cut_out)
{
    auto const procedural = kind == geometry_kind::quadrics;

    auto src = source_header(r, kind, "require raytracing_pipeline\n");

    // Restated rather than named as `tracer.path_rays`, since a hit group names a set of its own file.
    src += "rays path_rays:\n    surface: tracer.surface_payload\n    occlusion: tracer.shadow_payload\n\n";

    auto supplied_frame = cc::string();
    src += material_body(r, layout, supplied_frame, traced_spelling);

    if (procedural)
    {
        src += "@intersection fun sv_intersection(b: procedural_box){tracer.traced, tracer.bindless} -> "
               "report[tracer.quadric_attributes]:\n"
               "    return tracer.intersect_quadric(b)\n\n";
        cc::format_append(src,
                          "@closest_hit fun sv_closest_hit(h: procedural_hit[tracer.quadric_attributes], p: mut "
                          "tracer.surface_payload){{tracer.traced, tracer.bindless, slug.tables}}:\n"
                          "    let ctx = tracer.quadric_context(h)\n"
                          "    tracer.shade_quadric(h, mut p, sv_evaluate_material(ctx), {})\n\n",
                          supplied_frame);
        // The intersection is the whole row's, so a shadow ray meets the batch it traverses.
        src += "hit_group sv_material for path_rays:\n"
               "    geometry = .procedural\n"
               "    intersection = sv_intersection\n"
               "    surface = (closest_hit = sv_closest_hit)\n"
               "    occlusion = ()\n";
        return src;
    }

    cc::format_append(src,
                      "@closest_hit fun sv_closest_hit(h: triangle_hit, p: mut tracer.surface_payload){{tracer.traced, "
                      "tracer.bindless, slug.tables}}:\n"
                      "    let ctx = tracer.triangle_context(h)\n"
                      "    tracer.shade_triangle(h, mut p, ctx, sv_evaluate_material(ctx), {})\n\n",
                      supplied_frame);

    if (!can_cut_out)
    {
        src += "hit_group sv_material for path_rays:\n"
               "    surface = (closest_hit = sv_closest_hit)\n"
               "    occlusion = ()\n";
        return src;
    }

    // The cutout test twice, since an any hit takes the payload of the one ray type its record serves.
    // Neither reads the material's code: opacity comes through the instance's record, the same for every material.
    // Each draws from its payload: a surface ray from the path's stream as the ray left, a shadow ray from the seed its caller wrote.
    src += "@any_hit fun sv_any_hit(c: triangle_candidate, p: mut tracer.surface_payload){tracer.traced, "
           "tracer.bindless} -> hit_decision:\n"
           "    return tracer.cutout(c, tracer.cutout_seed(p.rng, tracer.cutout_ray_surface), "
           "tracer.candidate_opacity(c))\n\n";
    src += "@any_hit fun sv_shadow_any_hit(c: triangle_candidate, p: mut tracer.shadow_payload){tracer.traced, "
           "tracer.bindless} -> hit_decision:\n"
           "    return tracer.cutout(c, p.seed, tracer.candidate_opacity(c))\n\n";
    src += "hit_group sv_material for path_rays:\n"
           "    surface = (closest_hit = sv_closest_hit, any_hit = sv_any_hit)\n"
           "    occlusion = (any_hit = sv_shadow_any_hit)\n";
    return src;
}

/// The wavefront's shade kernel of one permutation: `sv_shade`, over this permutation's range of the sorted hits.
[[nodiscard]] cc::string generate_shade_kernel(resolved_material const& r,
                                               material_parameter_layout const& layout,
                                               geometry_kind kind)
{
    // The tracer's acceleration structure is in the group this lists, and it needs the pipeline wherever a device grants one.
    auto src = source_header(r, kind, "require ray_query\nrequire raytracing_pipeline\n");
    auto supplied_frame = cc::string();
    src += material_body(r, layout, supplied_frame, traced_spelling);

    src += "@compute(64) fun sv_shade(@thread_id id: int3){tracer.traced, tracer.bindless, slug.tables, "
           "tracer.wavefront_step}:\n"
           "    let slot = tracer.shade_slot(tracer.thread_index(id))\n"
           "    if slot == tracer.no_slot => return\n";
    if (kind == geometry_kind::quadrics)
        cc::format_append(src,
                          "    let ctx = tracer.stored_quadric_context(slot)\n"
                          "    tracer.shade_stored_quadric(slot, sv_evaluate_material(ctx), {})\n",
                          supplied_frame);
    else
        cc::format_append(src,
                          "    let ctx = tracer.stored_triangle_context(slot)\n"
                          "    tracer.shade_stored_triangle(slot, ctx, sv_evaluate_material(ctx), {})\n",
                          supplied_frame);
    return src;
}

/// The raster path's pixel stage of one triangle permutation: `sv_raster`, over the triangle module `raster`'s vertex stage drew.
///
/// The cutout reads the instance's opacity record through `tracer.instance_opacity`, the reader a hit's any-hit cuts through.
/// So a pixel cuts what a hit does wherever the type's cutoff is above 0; coverage, a cutoff of 0, is cut below one half (`raster.is_cut`).
/// The type's declared occlusion reaches `raster.shade` the same way, and 1 where it declares none.
[[nodiscard]] cc::string generate_raster_pixel(resolved_material const& r,
                                               material_parameter_layout const& layout,
                                               material_opacity const& opacity)
{
    auto src = source_header(r, geometry_kind::triangles, {}, "raster");
    auto supplied_frame = cc::string();
    src += material_body(r, layout, supplied_frame, raster_spelling);

    src += "@pixel fun sv_raster(p: raster.varyings){raster.drawn, tracer.bindless, slug.tables} -> raster.targets:\n"
           "    let ctx = raster.pixel_context(p)\n";
    if (opacity.can_cut_out())
        src += "    if raster.is_cut(p, ctx) => discard\n";
    auto const occlusion = r.type->occlusion_attribute.empty()
                             ? cc::string("1.0")
                             : cc::format("sv_attribute_{}(ctx)", r.type->occlusion_attribute);
    cc::format_append(src, "    return raster.shade(p, ctx, sv_evaluate_material(ctx), {}, {})\n", supplied_frame,
                      occlusion);
    return src;
}
} // namespace

cc::string sgl_palette_sampler(sg::sampler const& s)
{
    auto const address = [](sg::sampler_address_mode m) -> cc::string_view
    {
        switch (m)
        {
        case sg::sampler_address_mode::repeat:
            return "repeat";
        case sg::sampler_address_mode::mirror_repeat:
            return "mirror";
        case sg::sampler_address_mode::clamp_edge:
            return "clamp";
        }
        return "repeat";
    };
    auto const filter = s.mag_filter == sg::sampler_filter::nearest ? cc::string_view("nearest") : "linear";
    return cc::format("palette_{}_{}_{}", filter, address(s.address_u), address(s.address_v));
}

u32 palette_index(sg::sampler const& s)
{
    auto const address = [](sg::sampler_address_mode m) -> u32
    {
        switch (m)
        {
        case sg::sampler_address_mode::repeat:
            return 0;
        case sg::sampler_address_mode::mirror_repeat:
            return 1;
        case sg::sampler_address_mode::clamp_edge:
            return 2;
        }
        return 0;
    };
    auto const filter = s.mag_filter == sg::sampler_filter::nearest ? u32(0) : u32(1);
    return filter * 9 + address(s.address_u) * 3 + address(s.address_v);
}

cc::hash128 material_shader_key(cc::hash128 permutation_key, material_shader_options const& opts)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(permutation_key);
    b.add_pod(opts.kind);
    return cc::hash128::create(b.written_bytes(), impl::material_permutation_hash_seed);
}

cc::string_view sgl_type_of(attribute_format format)
{
    if (!format.is_scalar() && !format.is_vector())
        return {}; // a matrix has no settled raw-buffer layout here yet

    auto const scalar = [&]() -> cc::string_view
    {
        switch (format.scalar)
        {
        case scalar_type::f32:
            return "float";
        case scalar_type::i32:
            return "int";
        case scalar_type::u32:
            return "uint";
        default:
            return {};
        }
    }();
    if (scalar.empty())
        return {};

    switch (format.component_count())
    {
    case 1:
        return scalar;
    case 2:
        return scalar == "float" ? "float2" : (scalar == "int" ? "int2" : "uint2");
    case 3:
        return scalar == "float" ? "float3" : (scalar == "int" ? "int3" : "uint3");
    case 4:
        return scalar == "float" ? "float4" : (scalar == "int" ? "int4" : "uint4");
    default:
        return {};
    }
}

generated_material_shader generate_material_shader(resolved_material const& r, material_shader_options const& opts)
{
    CC_ASSERT(r.type != nullptr, "a resolved material names its type");

    for (auto const& a : r.attributes)
        CC_ASSERT(!sgl_type_of(a.format).empty(), "a material attribute must be a scalar or vector of f32 / i32 / "
                                                  "u32");

    auto layout = build_layout(r);
    auto const opacity = opacity_of(r, layout);
    auto const emission = emission_of(r, layout);

    auto source = generate_hit_group(r, layout, opts.kind, opacity.can_cut_out());
    auto shade_source = generate_shade_kernel(r, layout, opts.kind);
    // A quadric has no raster stage yet; issue: feat-sv-raster-quadric-batches
    auto raster_source = opts.kind == geometry_kind::triangles ? generate_raster_pixel(r, layout, opacity) : cc::string();
    return {.source = cc::move(source),
            .shade_source = cc::move(shade_source),
            .raster_source = cc::move(raster_source),
            .layout = cc::move(layout),
            .opacity = opacity,
            .emission = emission,
            .key = material_shader_key(r.permutation_key, opts)};
}
} // namespace sv
