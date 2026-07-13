#include "context.cached.hh"

#include <clean-core/thread/async.hh> // the async pipeline handles are cc::shared_async (copied here on return)
#include <shaped-graphics/context.hh>
#include <shaped-graphics/pipeline_cache.hh>

namespace sg
{
binding_group_layout_handle context_cached_scope::acquire_binding_group_layout(cc::span<binding const> bindings,
                                                                               cc::span<named_sampler const> static_samplers)
{
    return _ctx.pipeline_cache_ref().acquire_binding_group_layout(_ctx, bindings, static_samplers);
}

pipeline_layout_handle context_cached_scope::acquire_pipeline_layout(pipeline_layout_description const& desc)
{
    return _ctx.pipeline_cache_ref().acquire_pipeline_layout(_ctx, desc);
}

async_compute_pipeline context_cached_scope::acquire_compute_pipeline(compute_pipeline_description const& desc)
{
    return _ctx.pipeline_cache_ref().acquire_compute_pipeline(_ctx, desc);
}

async_raytracing_pipeline context_cached_scope::acquire_raytracing_pipeline(raytracing_pipeline_description const& desc)
{
    return _ctx.pipeline_cache_ref().acquire_raytracing_pipeline(_ctx, desc);
}

pipeline_cache& context_cached_scope::cache()
{
    return _ctx.pipeline_cache_ref();
}
} // namespace sg
