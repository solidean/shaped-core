#pragma once

#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-viewer/fwd.hh>

/// Fills an open scope's depth target from a float texture of clip-space depth, texel for texel.
///
/// What makes a raster draw after the trace occluded by it: the trace writes its primary-hit depth as a plain texture,
/// which a depth test cannot read, and this turns it into one it can.
/// The scope must have a depth target the size of `source`, and no color target.
/// One pipeline per depth format, built in the background: execute declines until it is ready.
class sv::depth_fill_routine : public sg::render_routine<depth_fill_routine, sg::pixel_format>
{
public:
    [[nodiscard]] static sg::routine_outcome execute(sg::rendering_scope& scope, sg::texture_2d const& source);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::binding_group_layout_handle _group_layout;
    sg::raster_pipeline_handle _pipeline;
};
