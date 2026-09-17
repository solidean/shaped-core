#include "material_shader_cache.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/material/material.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/material/resolve.hh>
#include <shaped-viewer/scene/resident_mesh.hh>
#include <shaped-viewer/shader_library.hh>

namespace sv
{
namespace
{
/// Where a generated source looks for its includes: sv's own package mount, which is what carries
/// `material_runtime.hlsli` and `pt_material_hit.hlsli`.
constexpr cc::string_view include_dir = "sv_shaders";

[[nodiscard]] sg::async_compiled_shader failed(cc::string message)
{
    return cc::make_async_from_error<sg::compiled_shader>(cc::async_error::make_error(cc::any_error(cc::move(message))));
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
                                                   "    surface.base_color = float3(0.5, 0.5, 0.5);\n"
                                                   "    surface.specular_roughness = 1.0;");
    static auto const material = sv::material::create("sv_fallback", material_type_id::invalid, {});

    // Resolved against the kind it will be generated for, even though an empty signature asks the geometry for nothing:
    // `acquire` states that `r` was resolved against a view of that same kind, and the fallback is the one caller that
    // would otherwise be exempt from the contract it is documented under.
    return resolve_material(type, material, geometry_view{.kind = kind});
}
} // namespace


material_shader_cache material_shader_cache::create(sg::shader_format format, material_shader_options const& opts)
{
    auto cache = material_shader_cache();
    cache._format = format;
    cache._entry_point = cc::string(opts.entry_point);
    cache._runtime_include = cc::string(opts.runtime_include);
    cache._epilogue_include = cc::string(opts.epilogue_include);
    if (opts.bindless != nullptr)
        cache._bindless = *opts.bindless;
    return cache;
}

material_shader_options material_shader_cache::generation_options() const
{
    return generation_options(geometry_kind::triangles);
}

material_shader_options material_shader_cache::generation_options(geometry_kind kind) const
{
    auto opts = material_shader_options{.entry_point = _entry_point,
                                        .runtime_include = _runtime_include,
                                        .epilogue_include = _epilogue_include,
                                        .bindless = &_bindless};

    // `entry_point` names the generated MATERIAL function and is the same either way; what the kind picks is the pair
    // of includes, which is exactly what `material_shader_key` folds in to keep the two spellings apart in one cache.
    if (kind == geometry_kind::quadrics)
    {
        opts.runtime_include = quadric_runtime_include;
        opts.epilogue_include = quadric_hit_epilogue_include;
    }
    return opts;
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
    // The kind is in the key through the options' two includes, so the same material on a mesh and on a quadric batch
    // is two entries here rather than a collision.
    auto const opts = generation_options(kind);
    auto const key = material_shader_key(r.permutation_key, opts);
    if (auto const* const resident = _by_key.get_ptr(key); resident != nullptr)
        return *resident;

    auto const procedural = kind == geometry_kind::quadrics;

    // The epilogue is what makes this a shader rather than a function: it defines the closest-hit that calls the material.
    auto generated = generate_material_shader(r, opts);

    // A cutout on a quadric batch is not merely unimplemented, it is unreachable: the instance is submitted with
    // `opaque_override`, so the hardware skips any-hit entirely and the material's own coverage never runs.
    // Said out loud rather than dropped, because the image is simply wrong and nothing else would explain it.
    if (procedural && generated.can_cut_out)
        CC_LOG_WARNING("material '{}' writes geometry_opacity, which a quadric batch cannot cut out with — the batch "
                       "is submitted opaque, so it will draw fully opaque",
                       r.type->name);

    auto lib = acquire_shader_library();
    auto shader
        = lib.has_error()
            ? failed(cc::format("shaped-viewer: no shader library to compile material '{}' through", r.type->name))
            : lib.value()->compile_source(
                  generated.source, sg::shader_stage::closest_hit,
                  procedural ? quadric_hit_entry_point : hit_entry_point, _format,
                  {.include_dir = include_dir,
                   .label = cc::format("<material '{}'{}>", r.type->name, procedural ? " quadric" : "")});

    // The same source at its other entry points, which is what keeps the stages from disagreeing about the layout
    // they read.
    //
    // Which other entry points there are is the kind's: a procedural permutation owes an INTERSECTION shader, since
    // that is what makes its hit group procedural and a procedural BLAS cannot be traced without one.
    // A triangle one owes the cutout test instead, and only where the material can cut out -- otherwise the any-hit
    // could reject nothing, and a hit group carrying one gives up the hardware's opaque path for every intersection.
    auto intersection = sg::async_compiled_shader();
    auto any_hit = sg::async_compiled_shader();
    auto shadow_any_hit = sg::async_compiled_shader();

    if (procedural)
    {
        intersection
            = lib.has_error()
                ? failed(cc::format("shaped-viewer: no shader library to compile material '{}' intersection through",
                                    r.type->name))
                : lib.value()->compile_source(
                      generated.source, sg::shader_stage::intersection, quadric_intersection_entry_point, _format,
                      {.include_dir = include_dir, .label = cc::format("<material '{}' intersection>", r.type->name)});
    }
    else if (generated.can_cut_out && lib.has_value())
    {
        any_hit = lib.value()->compile_source(
            generated.source, sg::shader_stage::any_hit, any_hit_entry_point, _format,
            {.include_dir = include_dir, .label = cc::format("<material '{}' any-hit>", r.type->name)});

        // The shadow record's copy, which differs only in the payload it declares.
        shadow_any_hit = lib.value()->compile_source(
            generated.source, sg::shader_stage::any_hit, shadow_any_hit_entry_point, _format,
            {.include_dir = include_dir, .label = cc::format("<material '{}' shadow any-hit>", r.type->name)});
    }

    auto entry = _by_key.entry(key);
    return entry.get_or_emplace(material_permutation{.key = generated.key,
                                                     .layout = cc::move(generated.layout),
                                                     .samplers = cc::move(generated.samplers),
                                                     .shader = cc::move(shader),
                                                     .any_hit = cc::move(any_hit),
                                                     .shadow_any_hit = cc::move(shadow_any_hit),
                                                     .intersection = cc::move(intersection),
                                                     .can_cut_out = !procedural && generated.can_cut_out,
                                                     .source = cc::move(generated.source)});
}
} // namespace sv
