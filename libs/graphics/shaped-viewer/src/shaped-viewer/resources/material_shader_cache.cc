#include "material_shader_cache.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/material/material_type.hh>
#include <shaped-viewer/material/resolve.hh>
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
