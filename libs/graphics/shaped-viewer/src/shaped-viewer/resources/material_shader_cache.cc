#include "material_shader_cache.hh"

#include <clean-core/common/assert.hh>
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
resolved_material fallback_resolution()
{
    static auto const type = material_type::create("sv_fallback", {},
                                                   "    surface.base_color = float3(0.5, 0.5, 0.5);\n"
                                                   "    surface.specular_roughness = 1.0;");
    static auto const material = sv::material::create("sv_fallback", material_type_id::invalid, {});

    // The mesh is what a resolve walks for candidates, and an empty signature asks it for nothing.
    static auto const mesh = sv::resident_mesh();
    return resolve_material(type, material, mesh);
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
    return {.entry_point = _entry_point,
            .runtime_include = _runtime_include,
            .epilogue_include = _epilogue_include,
            .bindless = &_bindless};
}


material_permutation const& material_shader_cache::acquire_fallback()
{
    return acquire(fallback_resolution());
}

material_shader_options material_shader_cache::quadric_generation_options() const
{
    auto opts = generation_options();
    opts.runtime_include = quadric_runtime_include;
    opts.epilogue_include = quadric_hit_epilogue_include;
    return opts;
}

material_permutation const& material_shader_cache::acquire_quadric_fallback()
{
    // The same material `acquire_fallback` uses, spelled against the quadric runtime and epilogue — which is the whole point
    // being demonstrated: one material, two geometry kinds, two permutations, one cache.
    auto const resolved = fallback_resolution();

    auto const opts = quadric_generation_options();
    auto const key = material_shader_key(resolved.permutation_key, opts);
    if (auto const* const resident = _by_key.get_ptr(key); resident != nullptr)
        return *resident;

    auto generated = generate_material_shader(resolved, opts);

    auto lib = acquire_shader_library();
    auto shader = sg::async_compiled_shader();
    auto intersection = sg::async_compiled_shader();

    if (lib.has_error())
    {
        shader = failed("shaped-viewer: no shader library to compile the quadric permutation through");
        intersection = failed("shaped-viewer: no shader library to compile the quadric intersection through");
    }
    else
    {
        shader = lib.value()->compile_source(generated.source, sg::shader_stage::closest_hit, quadric_hit_entry_point,
                                             _format, {.include_dir = include_dir, .label = "<quadric closest-hit>"});

        // The SAME source at its other entry point, exactly as the cutout any-hit is compiled from the material's.
        // One source per permutation is what keeps the two from disagreeing about the layout they read.
        intersection = lib.value()->compile_source(generated.source, sg::shader_stage::intersection,
                                                   quadric_intersection_entry_point, _format,
                                                   {.include_dir = include_dir, .label = "<quadric intersection>"});
    }

    auto entry = _by_key.entry(key);
    return entry.get_or_emplace(material_permutation{.key = generated.key,
                                                     .layout = cc::move(generated.layout),
                                                     .samplers = cc::move(generated.samplers),
                                                     .shader = cc::move(shader),
                                                     .intersection = cc::move(intersection),
                                                     .can_cut_out = false,
                                                     .source = cc::move(generated.source)});
}


material_permutation const* material_shader_cache::find(cc::hash128 key) const
{
    return _by_key.get_ptr(key);
}

material_permutation const& material_shader_cache::acquire(resolved_material const& r)
{
    // Computed rather than generated-then-read: a miss is what has to generate, and the key does not need the text.
    auto const opts = generation_options();
    auto const key = material_shader_key(r.permutation_key, opts);
    if (auto const* const resident = _by_key.get_ptr(key); resident != nullptr)
        return *resident;

    // The epilogue is what makes this a shader rather than a function: it defines the closest-hit that calls the material.
    auto generated = generate_material_shader(r, opts);

    auto lib = acquire_shader_library();
    auto shader
        = lib.has_error()
            ? failed(cc::format("shaped-viewer: no shader library to compile material '{}' through", r.type->name))
            : lib.value()->compile_source(
                  generated.source, sg::shader_stage::closest_hit, hit_entry_point, _format,
                  {.include_dir = include_dir, .label = cc::format("<material '{}'>", r.type->name)});

    // The same source, compiled a second time at its other entry point.
    // Only where the material can cut out: otherwise the any-hit could reject nothing, and a hit group carrying one gives up
    // the hardware's opaque path for every intersection on it.
    auto any_hit = sg::async_compiled_shader();
    auto shadow_any_hit = sg::async_compiled_shader();
    if (generated.can_cut_out && lib.has_value())
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
                                                     .can_cut_out = generated.can_cut_out,
                                                     .source = cc::move(generated.source)});
}
} // namespace sv
