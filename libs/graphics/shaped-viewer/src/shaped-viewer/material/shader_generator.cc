#include "shader_generator.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <shaped-viewer/material/impl/material_hash.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/resources/bindless_tables.hh>
#include <shaped-viewer/scene/mesh_attribute.hh>

namespace sv
{
namespace
{
constexpr i32 slot_alignment = 4;       ///< ByteAddressBuffer loads are 4-byte granular, so every slot starts on 4
constexpr i32 attribute_desc_size = 12; ///< sv::attribute_desc: buffer, offset, stride

[[nodiscard]] i32 align_up(i32 value, i32 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

/// The `sv::interpolate_*` / `sv::load_element_*` suffix for a component count.
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

/// The `.rgba` swizzle that narrows a sample to the attribute's component count.
[[nodiscard]] cc::string_view sample_swizzle(int components)
{
    switch (components)
    {
    case 1:
        return ".r";
    case 2:
        return ".rg";
    case 3:
        return ".rgb";
    case 4:
        return "";
    default:
        return "";
    }
}

/// The expression naming the three element indices a frequency reads, and whether it interpolates at all.
/// `per_triangle` is flat — one element for the whole primitive — so it loads rather than interpolates.
[[nodiscard]] bool interpolates(attribute_frequency f)
{
    return f != attribute_frequency::per_triangle;
}

[[nodiscard]] cc::string element_expression(attribute_frequency f)
{
    switch (f)
    {
    case attribute_frequency::per_vertex:
        return "ctx.corner";
    case attribute_frequency::per_corner:
        return "sv::corner_elements(ctx)";
    case attribute_frequency::per_triangle:
        return "ctx.primitive";
    default:
        CC_UNREACHABLE("a mesh attribute a material reads is per_vertex, per_corner or per_triangle");
    }
}

/// The bindless buffer expression for a descriptor, non-uniform because the index varies per instance.
[[nodiscard]] cc::string buffer_expression(cc::string_view desc)
{
    return cc::format("{}[NonUniformResourceIndex({}.buffer)]", name_of(bindless_table::buffers), desc);
}

/// Every distinct sampler this permutation samples with, in first-use order.
/// Distinct rather than one per attribute: two attributes sampled the same way share a `SamplerState`, and the sampler set is part
/// of the permutation, so this is stable for a given key.
[[nodiscard]] cc::vector<sg::sampler> distinct_samplers(resolved_material const& r)
{
    auto samplers = cc::vector<sg::sampler>();
    for (auto const& a : r.attributes)
    {
        if (a.sample == nullptr)
            continue;
        auto found = false;
        for (auto const& s : samplers)
            if (s == a.sample->sampler)
                found = true;
        if (!found)
            samplers.push_back(a.sample->sampler);
    }
    return samplers;
}

[[nodiscard]] i32 index_of_sampler(cc::span<sg::sampler const> samplers, sg::sampler const& s)
{
    for (auto i = 0; i < samplers.size(); ++i)
        if (samplers[i] == s)
            return i32(i);
    CC_UNREACHABLE("every sampler in the permutation was collected");
}

[[nodiscard]] bool samples_texture(resolved_material const& r)
{
    for (auto const& a : r.attributes)
        if (a.sample != nullptr)
            return true;
    return false;
}

/// Whether any attribute reaches a buffer — a mesh attribute, a uv set, or the parameter block itself.
/// The parameter block always does, so this is true whenever the signature is non-empty.
[[nodiscard]] bool reads_buffers(resolved_material const& r)
{
    return !r.attributes.empty();
}

/// Whether the generated source must declare the buffer table.
/// An epilogue counts even when no attribute reads a buffer: `pt_material_hit.hlsli` reads the mesh's positions through the table unconditionally,
/// so an attribute-less type would otherwise generate a shader that does not compile.
[[nodiscard]] bool declares_buffers(resolved_material const& r, material_shader_options const& opts)
{
    return reads_buffers(r) || !opts.epilogue_include.empty();
}

[[nodiscard]] u32 count_of(bindless_config const& cfg, bindless_table table)
{
    for (auto const& b : cfg.tables)
        if (b.table == table)
            return b.count;
    return 0;
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
        case material_frequency::mesh_texture:
            push(cc::string(a.name), material_slot_kind::texture_index, i32(sizeof(u32)), a.format, i32(i));
            push(cc::format("{}.uv", a.name), material_slot_kind::attribute_descriptor, attribute_desc_size,
                 attribute_format::of_vector(scalar_type::f32, 2), i32(i));
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
} // namespace

cc::hash128 material_shader_key(cc::hash128 permutation_key, material_shader_options const& opts)
{
    auto const defaults = bindless_config();
    auto const& bindless = opts.bindless != nullptr ? *opts.bindless : defaults;

    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(permutation_key);
    b.add_string(opts.entry_point);
    b.add_string(opts.runtime_include);
    b.add_string(opts.epilogue_include);

    // Each table's own enumerator alongside its count, so omitting a table and declaring it empty stay distinct.
    b.add_pod(i64(bindless.tables.size()));
    for (auto const& t : bindless.tables)
    {
        b.add_pod(t.table);
        b.add_pod(t.count);
    }
    return cc::hash128::create(b.written_bytes(), impl::material_permutation_hash_seed);
}

cc::string_view hlsl_type_of(attribute_format format)
{
    if (!format.is_scalar() && !format.is_vector())
        return {}; // a matrix has no settled ByteAddressBuffer layout here yet

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
        CC_ASSERT(!hlsl_type_of(a.format).empty(), "a material attribute must be a scalar or vector of f32 / i32 / "
                                                   "u32");

    auto const defaults = bindless_config();
    auto const& bindless = opts.bindless != nullptr ? *opts.bindless : defaults;

    auto const layout = build_layout(r);
    auto samplers = distinct_samplers(r);

    auto src = cc::string();
    cc::format_append(src, "// generated from material type '{}' — do not edit\n", r.type->name);
    cc::format_append(src, "#include \"{}\"\n\n", opts.runtime_include);

    // Only the tables this permutation touches, so the reflection a caller binds against stays as small as the material is.
    if (declares_buffers(r, opts))
        cc::format_append(src, "ByteAddressBuffer {}[{}] : register(t0, space{});\n", name_of(bindless_table::buffers),
                          count_of(bindless, bindless_table::buffers), space_of(bindless_table::buffers));
    if (samples_texture(r))
        cc::format_append(src, "Texture2D {}[{}] : register(t0, space{});\n", name_of(bindless_table::textures_2d),
                          count_of(bindless, bindless_table::textures_2d), space_of(bindless_table::textures_2d));
    for (auto i = 0; i < samplers.size(); ++i)
        cc::format_append(src, "SamplerState sv_sampler_{} : register(s{}, space0);\n", i, i);

    // Whether anything actually supplied each attribute, as a compile-time constant per permutation.
    //
    // A default is not a value somebody chose, and for some attributes those two must not shade the same way: an unsupplied
    // tangent frame has to fall back to the geometric one rather than to the identity rotation, which points at object-space
    // +z and is a frame belonging to no surface.
    // The resolution knows the difference and the fragment does not, so it is spelled here — for every attribute rather than
    // for the ones that happen to care, which keeps this a property of the generator rather than a list of names in it.
    for (auto const& a : r.attributes)
        cc::format_append(src, "#define SV_ATTR_SUPPLIED_{} {}\n", a.name,
                          a.frequency == material_frequency::material_type ? 0 : 1);

    cc::format_append(src, "\nsv::surface {}(sv::shading_context ctx)\n{{\n", opts.entry_point);
    cc::format_append(src, "    sv::surface surface = sv::default_surface();\n");

    // Every attribute is declared in the entry function's own scope and filled from the block below it.
    //
    // The block is what the material fragment's scope is protected by: the parameter buffer, an attribute's descriptor and a
    // sampled uv are all born and dead inside it, so the only names the fragment can collide with are its own attributes,
    // `surface` and `ctx`.
    // A zero initializer is what lets the value escape the block it is loaded in, and every attribute is a scalar or vector,
    // so the scalar broadcast covers all of them.
    for (auto const& a : r.attributes)
        cc::format_append(src, "    {} {} = 0;\n", hlsl_type_of(a.format), a.name);

    if (reads_buffers(r))
    {
        cc::format_append(
            src, "\n    {{\n        ByteAddressBuffer params = {}[NonUniformResourceIndex(ctx.param_buffer)];\n",
            name_of(bindless_table::buffers));

        for (auto i = 0; i < r.attributes.size(); ++i)
        {
            auto const& a = r.attributes[i];
            auto const components = a.format.component_count();

            switch (a.frequency)
            {
            case material_frequency::material_type:
            case material_frequency::material:
            case material_frequency::mesh_instance:
            {
                // A constant is read out of the parameter block whatever it is worth, which is why gold and copper share this
                // source.
                auto const& s = slot_for(layout, i32(i), material_slot_kind::constant);
                auto const load = components == 1 ? cc::string("Load") : cc::format("Load{}", components);
                auto const raw = cc::format("params.{}(ctx.param_offset + {})", load, s.offset);
                cc::format_append(src, "\n        {} = {};\n", a.name,
                                  a.format.scalar == scalar_type::f32 ? cc::format("asfloat({})", raw) : raw);
                break;
            }

            case material_frequency::mesh_attribute:
            {
                auto const& s = slot_for(layout, i32(i), material_slot_kind::attribute_descriptor);
                cc::format_append(src,
                                  "\n        {{\n"
                                  "            sv::attribute_desc desc = sv::load_attribute_desc(params, "
                                  "ctx.param_offset "
                                  "+ {});\n",
                                  s.offset);
                auto const buffer = buffer_expression("desc");
                if (interpolates(a.attribute->frequency))
                    // A rotation blends as one: the three corners are aligned into a common hemisphere before they are summed.
                    // Flat frequencies fall through to the plain load below, where there is nothing to blend and the mode
                    // therefore means nothing.
                    cc::format_append(
                        src, "            {} = sv::interpolate_{}({}, desc, {}, ctx.barycentrics);\n", a.name,
                        a.interpolation == attribute_interpolation::rotation ? cc::string("rotation")
                                                                             : cc::string(load_suffix(components)),
                        buffer, element_expression(a.attribute->frequency));
                else
                    cc::format_append(src, "            {} = sv::load_element_{}({}, desc, {});\n", a.name,
                                      load_suffix(components), buffer, element_expression(a.attribute->frequency));
                src += "        }\n";
                break;
            }

            case material_frequency::material_texture:
            case material_frequency::mesh_texture:
            {
                auto const& tex = slot_for(layout, i32(i), material_slot_kind::texture_index);
                auto const& uv_slot = slot_for(layout, i32(i), material_slot_kind::attribute_descriptor);
                cc::format_append(src,
                                  "\n        {{\n"
                                  "            sv::attribute_desc uv_desc = sv::load_attribute_desc(params, "
                                  "ctx.param_offset + {});\n",
                                  uv_slot.offset);

                auto const uv_buffer = buffer_expression("uv_desc");
                if (interpolates(a.uv->frequency))
                    cc::format_append(
                        src, "            float2 uv = sv::interpolate_f2({}, uv_desc, {}, ctx.barycentrics);\n",
                        uv_buffer, element_expression(a.uv->frequency));
                else
                    cc::format_append(src, "            float2 uv = sv::load_element_f2({}, uv_desc, {});\n", uv_buffer,
                                      element_expression(a.uv->frequency));

                cc::format_append(src, "            uint tex = params.Load(ctx.param_offset + {});\n", tex.offset);
                // SampleLevel rather than Sample: there are no derivatives in a ray tracing hit shader, so the mip has to be
                // named.
                cc::format_append(src,
                                  "            {} = {}[NonUniformResourceIndex(tex)].SampleLevel(sv_sampler_{}, uv, "
                                  "0){};\n"
                                  "        }}\n",
                                  a.name, name_of(bindless_table::textures_2d),
                                  index_of_sampler(samplers, a.sample->sampler), sample_swizzle(components));
                break;
            }
            }
        }

        src += "    }\n";
    }

    cc::format_append(src, "\n    // --- {} ---\n", r.type->name);
    src += r.type->shader;
    cc::format_append(src, "\n    return surface;\n}}\n");

    // After the entry function, so whatever it holds may call it.
    if (!opts.epilogue_include.empty())
        cc::format_append(src, "\n#include \"{}\"\n", opts.epilogue_include);

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

    return {.source = cc::move(src),
            .layout = cc::move(layout),
            .samplers = cc::move(samplers),
            .can_cut_out = can_cut_out,
            .key = material_shader_key(r.permutation_key, opts)};
}
} // namespace sv
