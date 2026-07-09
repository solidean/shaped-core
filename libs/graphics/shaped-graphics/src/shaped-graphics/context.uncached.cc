#include "context.uncached.hh"

#include <clean-core/common/utility.hh>
#include <shaped-graphics/compute_pipeline.hh> // compute_pipeline_description::shader
#include <shaped-graphics/context.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg
{
binding_layout_handle context_uncached_scope::create_binding_layout(cc::span<binding const> bindings,
                                                                    cc::span<named_sampler const> static_samplers)
{
    auto r = try_create_binding_layout(bindings, static_samplers);
    if (r.has_value())
        return cc::move(r.value());
    if (_ctx.is_device_lost())
        throw device_lost_exception(_ctx.device_loss_reason());
    throw pipeline_creation_exception("", r.error());
}

cc::result<binding_layout_handle> context_uncached_scope::try_create_binding_layout(
    cc::span<binding const> bindings,
    cc::span<named_sampler const> static_samplers)
{
    // Layouts have no transient variant — always persistent.
    return _ctx.try_create_binding_layout(bindings, static_samplers, lifetime_scope::persistent);
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
} // namespace sg
