#include "resolve.hh"

#include <clean-core/common/log.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/container/set.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-viewer/impl/content_hash.hh>
#include <shaped-viewer/material/impl/material_hash.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_library.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/scene/quadric_set.hh>
#include <shaped-viewer/scene/resident_mesh.hh>

namespace sv
{
geometry_view geometry_view::of(sv::resident_mesh const& mesh)
{
    return {.kind = geometry_kind::triangles, .attributes = mesh.attributes, .textures = mesh.textures};
}

geometry_view geometry_view::of(sv::resident_quadric_set const& set)
{
    // No textures, and that is the rule rather than an omission: see `serves`.
    return {.kind = geometry_kind::quadrics, .attributes = set.attributes};
}

bool serves(geometry_kind kind, attribute_frequency f)
{
    switch (f)
    {
    case attribute_frequency::per_instance:
        return true; // one value for the whole placement, which any geometry has

    case attribute_frequency::per_triangle:
        // One value per element of the geometry's own primitive stream, indexed by PrimitiveIndex().
        // Both geometries number that, and the generated load is the same for either — which is the whole reason the
        // frequencies are one set rather than one per kind.
        return true;

    case attribute_frequency::per_vertex:
    case attribute_frequency::per_corner:
    case attribute_frequency::per_edge:
        return kind == geometry_kind::triangles;
    }

    return false;
}

namespace
{
/// Says once that `name` was passed over because `kind` cannot number `f`.
///
/// Keyed on all three, because the same material on a mesh and on a batch is a different answer and both are worth hearing.
/// Unbounded in principle and bounded in practice: the set is one entry per attribute a material actually mismatched, which is
/// a handful, and it exists because resolution runs per placement per frame.
void warn_unservable_once(cc::string_view name, attribute_frequency f, geometry_kind kind)
{
    static auto said = cc::mutex<cc::set<cc::string>>();

    auto const key = cc::format("{}/{}/{}", name, int(f), int(kind));
    auto const first = said.lock([&](cc::set<cc::string>& s) { return s.insert(key); });
    if (!first)
        return;

    CC_LOG_WARNING("attribute '{}' is bound at a frequency {} geometry cannot number, so the material falls back to "
                   "its own constant for it",
                   name, kind == geometry_kind::quadrics ? "quadric" : "triangle");
}

/// The uv attribute a sample needs: two floats, indexed by something the geometry numbers.
/// A `per_instance` uv would be one coordinate for the whole mesh, which samples a single texel — so it does not count as carrying
/// uvs at all.
[[nodiscard]] mesh_attribute_binding const* find_uv_attribute(geometry_view const& geometry, cc::string_view name)
{
    // A quadric carries no uv at all, which is what makes both texture ranks unreachable on one.
    // It follows from the geometry rather than from a policy: a general quadric has no natural surface parameterization, so
    // there is no frequency a uv could be interpolated at.
    if (geometry.kind != geometry_kind::triangles)
        return nullptr;

    for (auto const& a : geometry.attributes)
        if (a.name == name && a.format == attribute_format::of_vector(scalar_type::f32, 2)
            && a.frequency != attribute_frequency::per_instance)
            return &a;
    return nullptr;
}

/// The mesh attribute named `name` at the declared format, at `per_instance` or at a geometric frequency.
/// A format mismatch resolves to null rather than asserting: the mesh and the material were authored apart, and the material is
/// what falls back.
[[nodiscard]] mesh_attribute_binding const* find_attribute(geometry_view const& geometry,
                                                           cc::string_view name,
                                                           attribute_format format,
                                                           bool per_instance)
{
    for (auto const& a : geometry.attributes)
    {
        if (a.name != name || a.format != format)
            continue;
        if ((a.frequency == attribute_frequency::per_instance) != per_instance)
            continue;

        // A frequency this geometry does not number is one more unusable candidate, so it loses to the coarser rank the way a
        // format mismatch does — which is what keeps a set authored for one geometry from failing on the other.
        //
        // Said out loud, because this is the one fallback a caller cannot see: a format mismatch is a mistake in the
        // attribute, and this is a mistake in pairing the attribute with the GEOMETRY.
        // The data is there, correctly formatted, and the drawing silently takes the material's constant instead.
        // Warned rather than refused, so an attribute list genuinely shared between a mesh and a batch still works —
        // and once per (name, frequency, kind), since resolution runs per placement and this sits on the frame path.
        if (!serves(geometry.kind, a.frequency))
        {
            warn_unservable_once(name, a.frequency, geometry.kind);
            continue;
        }

        return &a;
    }
    return nullptr;
}

[[nodiscard]] texture_sample_source const* find_mesh_texture(geometry_view const& geometry, cc::string_view name)
{
    for (auto const& t : geometry.textures)
        if (t.name == name)
            return &t.source;
    return nullptr;
}

/// Which rank supplies `d`, and what it supplies — the walk itself, without the declaration's own interpolation mode.
[[nodiscard]] resolved_attribute resolve_source(material_signature_entry const& d,
                                                material const& m,
                                                geometry_view const& mesh)
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
                      .frequency = material_frequency::mesh_texture_binding,
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
[[nodiscard]] resolved_attribute resolve_one(material_signature_entry const& d, material const& m, geometry_view const& mesh)
{
    auto r = resolve_source(d, m, mesh);
    r.interpolation = d.interpolation;
    return r;
}
} // namespace

resolved_material resolve_material(material_type const& type, material const& material, sv::resident_mesh const& mesh)
{
    return resolve_material(type, material, geometry_view::of(mesh));
}

resolved_material resolve_material(material_type const& type, material const& material, sv::resident_quadric_set const& set)
{
    return resolve_material(type, material, geometry_view::of(set));
}

resolved_material resolve_material(material_library const& lib, material_id id, sv::resident_quadric_set const& set)
{
    auto const& m = lib.get(id);
    return resolve_material(lib.get_type(m.type), m, set);
}

resolved_material resolve_material(material_type const& type, material const& material, geometry_view const& mesh)
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
        case material_frequency::mesh_texture_binding:
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

resolved_material resolve_material(material_library const& lib, material_id id, sv::resident_mesh const& mesh)
{
    auto const& m = lib.get(id);
    return resolve_material(lib.get_type(m.type), m, mesh);
}
} // namespace sv
