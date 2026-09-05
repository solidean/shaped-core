#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>        // cc::format
#include <shaped-graphics/binding/binding.hh> // binding::count
#include <shaped-graphics/binding/impl/binding_conflicts.hh>
#include <shaped-graphics/compute/compute_pipeline.hh> // compute_pipeline_description::shader
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/uncached.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>         // raster_pipeline_description
#include <shaped-graphics/raytracing/raytracing_pipeline.hh> // raytracing_pipeline_description

namespace sg
{
binding_group_layout_handle context_uncached_scope::create_binding_group_layout(cc::span<binding const> bindings,
                                                                                cc::span<named_sampler const> static_samplers)
{
    auto r = try_create_binding_group_layout(bindings, static_samplers);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw pipeline_creation_exception("", r.error());
}

cc::result<binding_group_layout_handle> context_uncached_scope::try_create_binding_group_layout(
    cc::span<binding const> bindings,
    cc::span<named_sampler const> static_samplers)
{
    // Rejected here rather than per backend, so an unbounded array fails identically everywhere: WebGPU has no
    // such thing, and sg holds the portable floor until it does.
    // See libs/graphics/shaped-graphics/docs/concepts/bindings.md.
    // An error rather than an assert — these bindings usually come from reflecting someone's shader, which
    // makes an unbounded array content, not a contract violation.
    for (auto const& b : bindings)
        if (b.count == 0)
            return cc::error(cc::format("binding_group_layout: '{}' is an unbounded array (count 0), which sg does "
                                        "not support — declare a bounded count and treat it as capacity",
                                        b.name));

    return _ctx.try_create_binding_group_layout(bindings, static_samplers, lifetime_scope::persistent);
}

pipeline_layout_handle context_uncached_scope::create_pipeline_layout(pipeline_layout_description const& desc)
{
    auto r = try_create_pipeline_layout(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw pipeline_creation_exception("", r.error());
}

cc::result<pipeline_layout_handle> context_uncached_scope::try_create_pipeline_layout(pipeline_layout_description const& desc)
{
    return _ctx.try_create_pipeline_layout(desc, lifetime_scope::persistent);
}

compute_pipeline_handle context_uncached_scope::create_compute_pipeline(compute_pipeline_description const& desc)
{
    auto r = try_create_compute_pipeline(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw pipeline_creation_exception(desc.shader.entry_point, r.error());
}

cc::result<compute_pipeline_handle> context_uncached_scope::try_create_compute_pipeline(
    compute_pipeline_description const& desc)
{
    return _ctx.try_create_compute_pipeline(desc, lifetime_scope::persistent);
}

raster_pipeline_handle context_uncached_scope::create_raster_pipeline(raster_pipeline_description const& desc)
{
    auto r = try_create_raster_pipeline(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw pipeline_creation_exception(desc.vertex_shader.entry_point, r.error());
}

cc::result<raster_pipeline_handle> context_uncached_scope::try_create_raster_pipeline(raster_pipeline_description const& desc)
{
    compiled_shader const* const stages[] = {
        &desc.vertex_shader,
        desc.fragment_shader.has_value() ? &desc.fragment_shader.value() : nullptr,
        desc.tessellation_control_shader.has_value() ? &desc.tessellation_control_shader.value() : nullptr,
        desc.tessellation_evaluation_shader.has_value() ? &desc.tessellation_evaluation_shader.value() : nullptr,
        desc.geometry_shader.has_value() ? &desc.geometry_shader.value() : nullptr,
    };
    if (auto conflict = impl::find_binding_conflict(stages); conflict.has_value())
        return cc::error(cc::move(conflict.value()));

    return _ctx.try_create_raster_pipeline(desc, lifetime_scope::persistent);
}

raytracing_pipeline_handle context_uncached_scope::create_raytracing_pipeline(raytracing_pipeline_description const& desc)
{
    auto r = try_create_raytracing_pipeline(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    cc::string const label = desc.raygen_shaders.empty() ? cc::string("") : desc.raygen_shaders.front().entry_point;
    throw pipeline_creation_exception(label, r.error());
}

cc::result<raytracing_pipeline_handle> context_uncached_scope::try_create_raytracing_pipeline(
    raytracing_pipeline_description const& desc)
{
    // Ray tracing is where this earns its keep: a pipeline's shaders naturally live in separate files, so nothing
    // but a shared header makes them agree about a group's numbering.
    cc::vector<compiled_shader const*> stages;
    for (auto const& s : desc.raygen_shaders)
        stages.push_back(&s);
    for (auto const& s : desc.miss_shaders)
        stages.push_back(&s);
    for (auto const& s : desc.callable_shaders)
        stages.push_back(&s);
    for (auto const& g : desc.hit_shaders)
    {
        if (g.closest_hit.has_value())
            stages.push_back(&g.closest_hit.value());
        if (g.any_hit.has_value())
            stages.push_back(&g.any_hit.value());
        if (g.intersection.has_value())
            stages.push_back(&g.intersection.value());
    }
    if (auto conflict = impl::find_binding_conflict(stages); conflict.has_value())
        return cc::error(cc::move(conflict.value()));

    return _ctx.try_create_raytracing_pipeline(desc, lifetime_scope::persistent);
}

raytracing_shader_table_handle context_uncached_scope::create_raytracing_shader_table(
    raytracing_shader_table_description const& desc)
{
    auto r = try_create_raytracing_shader_table(desc);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw pipeline_creation_exception("raytracing_shader_table", r.error());
}

cc::result<raytracing_shader_table_handle> context_uncached_scope::try_create_raytracing_shader_table(
    raytracing_shader_table_description const& desc)
{
    return _ctx.try_create_raytracing_shader_table(desc, lifetime_scope::persistent);
}
} // namespace sg
