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
/// The entry point every generated closest-hit defines, in `shaders/pt_material_hit.hlsli`.
constexpr cc::string_view hit_entry_point = "PtClosestHit";

/// Where a generated source looks for its includes: sv's own package mount, which is what carries
/// `material_runtime.hlsli` and `pt_material_hit.hlsli`.
constexpr cc::string_view include_dir = "sv_shaders";

[[nodiscard]] sg::async_compiled_shader failed(cc::string message)
{
    return cc::make_async_from_error<sg::compiled_shader>(cc::async_error::make_error(cc::any_error(cc::move(message))));
}
} // namespace

material_shader_cache material_shader_cache::create(sg::shader_format format)
{
    auto cache = material_shader_cache();
    cache._format = format;
    return cache;
}

material_permutation const* material_shader_cache::find(cc::hash128 key) const
{
    return _by_key.get_ptr(key);
}

material_permutation const& material_shader_cache::acquire(resolved_material const& r)
{
    if (auto const* const resident = _by_key.get_ptr(r.permutation_key); resident != nullptr)
        return *resident;

    // The epilogue is what makes this a shader rather than a function: it defines the closest-hit that calls the material.
    auto generated = generate_material_shader(r, {.epilogue_include = "pt_material_hit.hlsli"});
    CC_ASSERT(generated.key == r.permutation_key, "a generated permutation is keyed by the resolution it came from");

    auto lib = acquire_shader_library();
    auto shader
        = lib.has_error()
            ? failed(cc::format("shaped-viewer: no shader library to compile material '{}' through", r.type->name))
            : lib.value()->compile_source(
                  generated.source, sg::shader_stage::closest_hit, hit_entry_point, _format,
                  {.include_dir = include_dir, .label = cc::format("<material '{}'>", r.type->name)});

    auto entry = _by_key.entry(r.permutation_key);
    return entry.get_or_emplace(material_permutation{.key = generated.key,
                                                     .layout = cc::move(generated.layout),
                                                     .samplers = cc::move(generated.samplers),
                                                     .shader = cc::move(shader),
                                                     .source = cc::move(generated.source)});
}
} // namespace sv
