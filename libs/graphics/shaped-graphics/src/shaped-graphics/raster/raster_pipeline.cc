#include <clean-core/common/log.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

namespace sg
{
raster_pipeline::~raster_pipeline() = default;

void impl::finish_raster_pipeline(raster_pipeline& pipeline, raster_pipeline_description const& desc)
{
    pipeline._target_set = desc.target_set;

    // Such a pipeline still builds, and draws as if the state were off.
    auto const& ds = desc.depth_stencil;
    if ((ds.depth_test || ds.depth_write || ds.stencil_test) && desc.depth_stencil_format == pixel_format::undefined)
        CC_LOG_WARNING("a raster pipeline for '{}' tests depth or stencil and has no depth_stencil_format, "
                       "so it draws without either",
                       desc.vertex_shader.entry_point);
}
} // namespace sg
