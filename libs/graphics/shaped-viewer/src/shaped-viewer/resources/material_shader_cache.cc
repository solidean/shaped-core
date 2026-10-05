#include "material_shader_cache.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <shaped-shader-library/raytracing_pipeline.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/material/resolve.hh>
#include <shaped-viewer/scene/resident_mesh.hh>
#include <shaped-viewer/shader_library.hh>
#include <sv_shaders.hh>

namespace sv
{
namespace
{
[[nodiscard]] cc::shared_async<cc::vector<sg::hit_shader>> failed_hit_group(cc::string message)
{
    return cc::make_async_from_error<cc::vector<sg::hit_shader>>(
        cc::async_error::make_error(cc::any_error(cc::move(message))));
}

/// The neutral material both fallbacks are built from: an EMPTY signature, which is the whole trick.
///
/// With no attributes there is no parameter block to read, so one hit group is valid for an instance whose block was laid out
/// for something else entirely.
/// Static because a `resolved_material` borrows its type and its material, and these have to outlive the resolve; the name is
/// what the content hash is over, so nothing else can collide with them.
resolved_material fallback_resolution(geometry_kind kind)
{
    static auto const type = material_type::create("sv_fallback", {},
                                                   "surface.base_color = float3(0.5, 0.5, 0.5)\n"
                                                   "surface.specular_roughness = 1.0\n");
    static auto const material = sv::material::create("sv_fallback", material_type_id::invalid, {});

    // Resolved against the kind it will be generated for, even though an empty signature asks the geometry for nothing:
    // `acquire` states that `r` was resolved against a view of that same kind, and the fallback is the one caller that
    // would otherwise be exempt from the contract it is documented under.
    return resolve_material(type, material, geometry_view{.kind = kind});
}
} // namespace


material_shader_cache material_shader_cache::create(sg::context* ctx)
{
    auto cache = material_shader_cache();
    cache._context = ctx;
    return cache;
}

material_permutation const& material_shader_cache::acquire_fallback(geometry_kind kind)
{
    return acquire(fallback_resolution(kind), kind);
}

material_permutation const& material_shader_cache::acquire_quadric_fallback()
{
    return acquire_fallback(geometry_kind::quadrics);
}

material_permutation const& material_shader_cache::acquire_quadric(resolved_material const& r)
{
    return acquire(r, geometry_kind::quadrics);
}

material_permutation const* material_shader_cache::find(cc::hash128 key) const
{
    return _by_key.get_ptr(key);
}


material_permutation const& material_shader_cache::acquire(resolved_material const& r, geometry_kind kind)
{
    // Computed rather than generated-then-read: a miss is what has to generate, and the key does not need the text.
    // The kind is in the key, so the same material on a mesh and on a quadric batch is two entries here rather than a
    // collision.
    auto const opts = material_shader_options{.kind = kind};
    auto const key = material_shader_key(r.permutation_key, opts);
    if (auto const* const resident = _by_key.get_ptr(key); resident != nullptr)
        return *resident;

    auto const procedural = kind == geometry_kind::quadrics;
    auto generated = generate_material_shader(r, opts);

    // A cutout on a quadric batch is not merely unimplemented, it is unreachable: the instance is submitted with
    // `opaque_override`, so the hardware skips any-hit entirely and the material's own coverage never runs.
    // Said out loud rather than dropped, because the image is simply wrong and nothing else would explain it.
    if (procedural && generated.can_cut_out)
        CC_LOG_WARNING("material '{}' writes geometry_opacity, which a quadric batch cannot cut out with — the batch "
                       "is submitted opaque, so it will draw fully opaque",
                       r.type->name);

    // Left cold: the tracer starts it the first time a trace names this permutation.
    auto label = cc::format("<material '{}'{}>", r.type->name, procedural ? " quadric" : "");
    auto lib = acquire_shader_library();
    auto hit_group = cc::shared_async<cc::vector<sg::hit_shader>>();
    if (_context == nullptr)
        hit_group = failed_hit_group(cc::format("no context to compile material '{}' through", r.type->name));
    else if (lib.has_error())
        hit_group = failed_hit_group(cc::format("no shader library to compile material '{}' through: {}", r.type->name,
                                                lib.error().to_string()));
    else
        hit_group = slib::compile_hit_group(_context, lib.value(), &sv::shaders::tracer_pipeline_path_t::definition(),
                                            generated.source, "sv_material", label);

    auto entry = _by_key.entry(key);
    return entry.get_or_emplace(material_permutation{.key = generated.key,
                                                     .layout = cc::move(generated.layout),
                                                     .kind = kind,
                                                     .can_cut_out = !procedural && generated.can_cut_out,
                                                     .hit_group = cc::move(hit_group),
                                                     .source = cc::move(generated.source),
                                                     .label = cc::move(label)});
}
} // namespace sv
