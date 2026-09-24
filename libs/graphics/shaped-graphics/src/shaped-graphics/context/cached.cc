#include <clean-core/thread/async.hh> // the async pipeline handles are cc::shared_async (copied here on return)
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/context/cached.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/pipeline_cache.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

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

async_raster_pipeline context_cached_scope::acquire_raster_pipeline(raster_pipeline_description const& desc)
{
    return _ctx.pipeline_cache_ref().acquire_raster_pipeline(_ctx, desc);
}

async_raster_pipeline context_cached_scope::acquire_raster_pipeline(cc::shared_async<raster_pipeline_description> desc)
{
    auto const& described = co_await desc;
    co_return co_await acquire_raster_pipeline(described);
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
