#include <clean-core/thread/async.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-rendering/keyed_pipeline_cache.hh>

namespace sr
{
cc::result<sg::raster_pipeline_handle> build_cached_raster_pipeline(sg::context& ctx,
                                                                    sg::raster_pipeline_description const& desc)
{
    auto result = cc::try_async_blocking_get(ctx.cached.acquire_raster_pipeline(desc));
    if (result.has_error())
        return cc::error(cc::move(result.error().underlying()));
    return cc::move(result).value();
}
} // namespace sr
