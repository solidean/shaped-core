#include <shaped-graphics/raster/raster_pipeline.hh>

namespace sg
{
raster_pipeline::~raster_pipeline() = default;

void impl::set_target_set(raster_pipeline const& pipeline, cc::string_view target_set)
{
    // Only ever called on a pipeline nobody else holds yet, and never on one created const.
    const_cast<raster_pipeline&>(pipeline)._target_set = target_set;
}
} // namespace sg
