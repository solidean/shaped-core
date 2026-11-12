#include <shaped-graphics/raster/raster_pipeline.hh>

namespace sg
{
raster_pipeline::~raster_pipeline() = default;

void impl::set_footprint(raster_pipeline const& pipeline, impl::pipeline_footprint footprint)
{
    // Only ever called on a pipeline nobody else holds yet, and never on one created const.
    const_cast<raster_pipeline&>(pipeline)._footprint = cc::move(footprint);
}

void impl::set_targets(raster_pipeline const& pipeline,
                       cc::string_view target_set,
                       raster_target_formats const& formats,
                       bool is_mesh_shaded)
{
    // Only ever called on a pipeline nobody else holds yet, and never on one created const.
    auto& p = const_cast<raster_pipeline&>(pipeline);
    p._target_set = target_set;
    p._target_formats = formats;
    p._is_mesh_shaded = is_mesh_shaded;
}

void impl::set_mesh_polyfill(raster_pipeline const& pipeline, mesh_polyfill_state state)
{
    // as set_targets: only on a pipeline nobody else holds yet
    auto& p = const_cast<raster_pipeline&>(pipeline);
    // a polyfilled geometry stage draws with `draw`, as its pipeline would natively
    p._is_mesh_shaded = state.input_corners == 0;
    p._mesh_polyfill = cc::move(state);
}

void impl::set_tessellation_polyfill(raster_pipeline const& pipeline, tessellation_polyfill_state state)
{
    // as set_targets: only on a pipeline nobody else holds yet
    auto& p = const_cast<raster_pipeline&>(pipeline);
    p._tessellation_polyfill = cc::move(state);
}
} // namespace sg
