#include "resolve.hh"

#include <clean-core/container/byte_stream_builder.hh>
#include <shaped-viewer/impl/content_hash.hh>
#include <shaped-viewer/material/impl/material_hash.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/scene/mesh.hh>

namespace sv
{
namespace
{
/// The uv attribute a sample needs: two floats, indexed by something the geometry numbers.
/// A `per_instance` uv would be one coordinate for the whole mesh, which samples a single texel — so it does not count as carrying
/// uvs at all.
[[nodiscard]] mesh_attribute_binding const* find_uv_attribute(sv::mesh const& mesh, cc::string_view name)
{
    for (auto const& a : mesh.attributes)
        if (a.name == name && a.format == attribute_format::of_vector(scalar_type::f32, 2)
            && a.frequency != attribute_frequency::per_instance)
            return &a;
    return nullptr;
}

/// The mesh attribute named `name` at the declared format, at `per_instance` or at a geometric frequency.
/// A format mismatch resolves to null rather than asserting: the mesh and the material were authored apart, and the material is
/// what falls back.
[[nodiscard]] mesh_attribute_binding const* find_attribute(sv::mesh const& mesh,
                                                           cc::string_view name,
                                                           attribute_format format,
                                                           bool per_instance)
{
    for (auto const& a : mesh.attributes)
    {
        if (a.name != name || a.format != format)
            continue;
        if ((a.frequency == attribute_frequency::per_instance) == per_instance)
            return &a;
    }
    return nullptr;
}

[[nodiscard]] texture_sample_source const* find_mesh_texture(sv::mesh const& mesh, cc::string_view name)
{
    for (auto const& t : mesh.textures)
        if (t.name == name)
            return &t.source;
    return nullptr;
}

/// Which rank supplies `d`, and what it supplies — the walk itself, without the declaration's own interpolation mode.
[[nodiscard]] resolved_attribute resolve_source(material_signature_entry const& d, material const& m, sv::mesh const& mesh)
{
    auto winner = resolved_attribute{.name = d.name,
                                     .format = d.format,
                                     .frequency = material_frequency::material_type,
                                     .constant = d.default_value};
    if (d.is_final)
        return winner;

    // The best constant seen so far, carried alongside the walk rather than recovered from it.
    // A sample overwrites the winner, and this is what survives to seed the placeholder it is drawn as until it lands.
    cc::span<byte const> best_constant = d.default_value;

    auto const* const binding = m.find(d.name);
    auto const binds_constant = binding != nullptr && binding->kind == material_source_kind::constant;
    auto const binds_texture = binding != nullptr && binding->kind == material_source_kind::texture_sample;

    if (binds_constant)
    {
        winner = {.name = d.name,
                  .format = d.format,
                  .frequency = material_frequency::material,
                  .constant = binding->constant};
        best_constant = binding->constant;
        if (binding->is_final)
            return winner;
    }

    if (auto const* const a = find_attribute(mesh, d.name, d.format, true); a != nullptr)
    {
        winner = {.name = d.name,
                  .format = d.format,
                  .frequency = material_frequency::mesh_instance,
                  .constant = a->value.span()};
        best_constant = a->value.span();
    }

    if (auto const* const a = find_attribute(mesh, d.name, d.format, false); a != nullptr)
        winner = {.name = d.name, .format = d.format, .frequency = material_frequency::mesh_attribute, .attribute = a};

    if (binds_texture)
    {
        if (auto const* const uv = find_uv_attribute(mesh, binding->sample.uv_attribute); uv != nullptr)
            winner = {.name = d.name,
                      .format = d.format,
                      .frequency = material_frequency::material_texture,
                      .fallback_constant = best_constant,
                      .sample = &binding->sample,
                      .uv = uv};

        // `final` stops the walk whether or not the sample itself was usable.
        // A binding the mesh carries no uv set for still refuses the mesh's texture, and what stands is the coarser rank that
        // had won — which is the whole point of refusing a texture we know to be bad.
        if (binding->is_final)
            return winner;
    }

    if (auto const* const t = find_mesh_texture(mesh, d.name); t != nullptr)
        if (auto const* const uv = find_uv_attribute(mesh, t->uv_attribute); uv != nullptr)
            winner = {.name = d.name,
                      .format = d.format,
                      .frequency = material_frequency::mesh_texture,
                      .fallback_constant = best_constant,
                      .sample = t,
                      .uv = uv};

    return winner;
}

/// One declaration walked down the chain, coarsest rank first.
/// Every step overwrites the winner, and a `final` one stops the walk — so the last rank that supplied a value is what comes back.
///
/// The interpolation mode is set once, after the walk: it is the declaration's whatever rank won, and none of the branches
/// above has any say in it.
[[nodiscard]] resolved_attribute resolve_one(material_signature_entry const& d, material const& m, sv::mesh const& mesh)
{
    auto r = resolve_source(d, m, mesh);
    r.interpolation = d.interpolation;
    return r;
}
} // namespace

resolved_material resolve_material(material_type const& type, material const& material, sv::mesh const& mesh)
{
    auto r = resolved_material{.type = &type, .source = &material};
    r.attributes.reserve(type.signature.size());
    for (auto const& d : type.signature)
        r.attributes.push_back(resolve_one(d, material, mesh));

    // The two keys are built in one pass over the same values, which is what keeps them from drifting apart.
    // Neither builder is the thread-local scratch: both are live at once, and that one may not be nested.
    auto shape = cc::byte_stream_builder();
    auto values = cc::byte_stream_builder();
    shape.add_pod(type.hash);
    values.add_pod(type.hash);

    for (auto const& a : r.attributes)
    {
        shape.add_string(a.name);
        shape.add_pod(a.format);
        shape.add_pod(a.frequency);
        values.add_pod(a.frequency);

        switch (a.frequency)
        {
        case material_frequency::material_type:
        case material_frequency::material:
        case material_frequency::mesh_instance:
            // Nothing shape-side: a constant is read out of the parameter block whatever it is worth, which is exactly why two
            // materials differing only in their constants share one shader.
            values.add_pod_span_sized(a.constant);
            break;

        case material_frequency::mesh_attribute:
            // The geometric frequency picks the load code, so it is shape; which buffer it lands in is value.
            shape.add_pod(a.attribute->frequency);
            values.add_pod(a.attribute->hash);
            break;

        case material_frequency::material_texture:
        case material_frequency::mesh_texture:
            shape.add_string(a.sample->uv_attribute);
            // The uv attribute's own frequency picks its load code, exactly as a directly-sourced attribute's does.
            shape.add_pod(a.uv->frequency);
            impl::add_sampler(shape, a.sample->sampler);
            // The swizzle is generated code rather than a value, so it belongs in the shape — and only as far as it is read.
            impl::add_swizzle(shape, a.sample->swizzle, a.format.component_count());

            // The transform splits the other way: whether there is one at all decides the generated code, and what it
            // scales by is a value the block carries — so a normal map's scale never forks a permutation.
            shape.add_pod(a.sample->transform.is_identity(a.format.component_count()));
            values.add_pod(a.sample->transform.scale);
            values.add_pod(a.sample->transform.bias);
            values.add_pod(a.sample->texture);
            values.add_pod(a.uv->hash);
            // The seed is a value like any other: two materials whose maps have not landed show different placeholders
            // and therefore fill their blocks differently, so they cannot share one.
            values.add_pod_span_sized(a.fallback_constant);
            break;
        }
    }

    r.permutation_key = cc::hash128::create(shape.written_bytes(), impl::material_permutation_hash_seed);
    r.parameter_key = cc::hash128::create(values.written_bytes(), impl::material_parameter_hash_seed);
    return r;
}

resolved_material resolve_material(material_library const& lib, material_id id, sv::mesh const& mesh)
{
    auto const& m = lib.get(id);
    return resolve_material(lib.get_type(m.type), m, mesh);
}
} // namespace sv
