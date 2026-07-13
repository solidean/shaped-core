#include "context.uncached.hh"

#include <clean-core/common/utility.hh>
#include <shaped-graphics/compute_pipeline.hh> // compute_pipeline_description::shader
#include <shaped-graphics/context.hh>
#include <shaped-graphics/exceptions.hh>
#include <shaped-graphics/raster_pipeline.hh>     // raster_pipeline_description
#include <shaped-graphics/raytracing_pipeline.hh> // raytracing_pipeline_description

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
    // Layouts have no transient variant — always persistent.
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
    // Layouts have no transient variant — always persistent.
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
    // Pipelines have no transient variant — always persistent.
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
    // Pipelines have no transient variant — always persistent.
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
    // Pipelines have no transient variant — always persistent.
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
    // A shader table is a persistent, uncached object referencing a specific pipeline.
    return _ctx.try_create_raytracing_shader_table(desc, lifetime_scope::persistent);
}
} // namespace sg
