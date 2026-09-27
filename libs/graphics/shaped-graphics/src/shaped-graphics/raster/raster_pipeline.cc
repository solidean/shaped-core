#include <shaped-graphics/raster/raster_pipeline.hh>

namespace sg
{
raster_pipeline::~raster_pipeline() = default;

void impl::set_footprint(raster_pipeline const& pipeline, impl::pipeline_footprint footprint)
{
    // Only ever called on a pipeline nobody else holds yet, and never on one created const.
    const_cast<raster_pipeline&>(pipeline)._footprint = cc::move(footprint);
}

void impl::set_targets(raster_pipeline const& pipeline, cc::string_view target_set, raster_target_formats const& formats)
{
    // Only ever called on a pipeline nobody else holds yet, and never on one created const.
    auto& p = const_cast<raster_pipeline&>(pipeline);
    p._target_set = target_set;
    p._target_formats = formats;
}
} // namespace sg
