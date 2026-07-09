#include "context.cached.hh"

#include <shaped-graphics/context.hh>
#include <shaped-graphics/pipeline_cache.hh>

namespace sg
{
binding_layout_handle context_cached_scope::acquire_binding_layout(cc::span<binding const> bindings,
                                                                   cc::span<named_sampler const> static_samplers)
{
    return _ctx.pipeline_cache_ref().acquire_binding_layout(_ctx, bindings, static_samplers);
}

async_compute_pipeline context_cached_scope::acquire_compute_pipeline(compute_pipeline_description const& desc)
{
    return _ctx.pipeline_cache_ref().acquire_compute_pipeline(_ctx, desc);
}

pipeline_cache& context_cached_scope::cache()
{
    return _ctx.pipeline_cache_ref();
}
} // namespace sg
