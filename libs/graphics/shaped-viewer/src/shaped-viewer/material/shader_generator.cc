#include "shader_generator.hh"

#include <clean-core/common/assert.hh>
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

[[nodiscard]] bool samples_texture(resolved_material const& r)
{
    for (auto const& a : r.attributes)
        if (a.sample != nullptr)
            return true;
    return false;
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

/// `fun sv_attribute_<name>(ctx)`: the load of attribute `index`, out of the parameter block, a mesh attribute or a texture.
[[nodiscard]] cc::string attribute_function(resolved_material const& r, material_parameter_layout const& layout, i32 index)
{
    auto const& a = r.attributes[index];
    auto const components = a.format.component_count();
    auto const samples = a.frequency == material_frequency::material_texture
                      || a.frequency == material_frequency::mesh_texture_binding;

    auto out = cc::string();
    cc::format_append(out, "fun sv_attribute_{}(ctx: material.shading_context){{{}}} -> {}:\n", a.name,
                      samples ? "tracer.traced, tracer.bindless" : "tracer.bindless", sgl_type_of(a.format));
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
        // An explicit level, since a hit has no derivatives; the texel is named because a constant selector reads it twice.
        cc::format_append(
            out, "    let texel = tracer.bindless.{}[nonuniform image].sample(uv, tracer.traced.{}, level = 0.0)\n",
            "textures_2d", sgl_palette_sampler(a.sample->sampler));

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

/// The hit group of one permutation; `generate_material_shader`'s header says what it holds.
[[nodiscard]] cc::string generate_hit_group(resolved_material const& r,
                                            material_parameter_layout const& layout,
                                            geometry_kind kind,
                                            bool can_cut_out)
{
    auto const procedural = kind == geometry_kind::quadrics;

    auto src = cc::string();
    cc::format_append(src, "// generated from material type '{}' — do not edit\n", r.type->name);
    src += "require raytracing_pipeline\n\n";
    src += "use material\nuse openpbr\n";
    if (procedural)
        src += "use quadric\n";
    src += "use tracer\n\n";

    // Restated rather than named as `tracer.path_rays`, since a hit group names a set of its own file.
    src += "rays path_rays:\n    surface: tracer.surface_payload\n    occlusion: tracer.shadow_payload\n\n";

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
        src += attribute_function(r, layout, i32(i));
        src += "\n";
    }

    auto const samples = samples_texture(r);
    cc::format_append(src, "fun sv_evaluate_material(ctx: material.shading_context){{{}}} -> openpbr.surface:\n",
                      samples ? "tracer.traced, tracer.bindless" : "tracer.bindless");
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
    src += "    return surface\n\n";

    // The tangent frame is the one supplied attribute the shading itself asks after, and a type that declares none supplies none.
    auto supplied_frame = cc::string("false");
    for (auto const& a : r.attributes)
        if (a.name == "tangent_frame")
            supplied_frame = "sv_supplied_tangent_frame";

    if (procedural)
    {
        src += "@intersection fun sv_intersection(b: procedural_box){tracer.traced, tracer.bindless} -> "
               "report[tracer.quadric_attributes]:\n"
               "    return tracer.intersect_quadric(b)\n\n";
        cc::format_append(src,
                          "@closest_hit fun sv_closest_hit(h: procedural_hit[tracer.quadric_attributes], p: mut "
                          "tracer.surface_payload){{tracer.traced, tracer.bindless}}:\n"
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
                      "tracer.bindless}}:\n"
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
    src += "@any_hit fun sv_any_hit(c: triangle_candidate, p: mut tracer.surface_payload, @launch_id id: "
           "int3){tracer.traced, "
           "tracer.bindless} -> hit_decision:\n"
           "    return tracer.cutout(c, id, sv_evaluate_material(tracer.candidate_context(c)).geometry_opacity)\n\n";
    src += "@any_hit fun sv_shadow_any_hit(c: triangle_candidate, p: mut tracer.shadow_payload, @launch_id id: "
           "int3){tracer.traced, "
           "tracer.bindless} -> hit_decision:\n"
           "    return tracer.cutout(c, id, sv_evaluate_material(tracer.candidate_context(c)).geometry_opacity)\n\n";
    src += "hit_group sv_material for path_rays:\n"
           "    surface = (closest_hit = sv_closest_hit, any_hit = sv_any_hit)\n"
           "    occlusion = (any_hit = sv_shadow_any_hit)\n";
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

    // Whether THIS permutation can reject an intersection, which is narrower than whether its type could.
    //
    // The type names the attribute its fragment writes `geometry_opacity` from, and that attribute's resolved frequency
    // is what decides the answer.
    // `material_type` frequency means nothing supplied it, so the fragment clamps the signature's own default and the
    // value is a compile-time constant.
    // An any-hit there would run at every intersection to reject nothing, and cost the instance its opaque fast path.
    auto const can_cut_out = [&]
    {
        if (r.type->opacity_attribute.empty())
            return false;

        for (auto const& a : r.attributes)
            if (a.name == r.type->opacity_attribute)
                return a.frequency != material_frequency::material_type;
        return false;
    }();

    auto source = generate_hit_group(r, layout, opts.kind, can_cut_out);
    return {.source = cc::move(source),
            .layout = cc::move(layout),
            .can_cut_out = can_cut_out,
            .key = material_shader_key(r.permutation_key, opts)};
}
} // namespace sv
