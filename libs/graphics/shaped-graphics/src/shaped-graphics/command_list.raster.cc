#include <clean-core/common/utility.hh> // cc::move
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/command_list.raster.hh>

namespace sg
{
color_target clear(render_target_view view, tg::vec4f color)
{
    return {.view = cc::move(view), .op = target_op::clear, .clear_color = color};
}

color_target keep(render_target_view view)
{
    return {.view = cc::move(view), .op = target_op::keep};
}

color_target discard(render_target_view view)
{
    return {.view = cc::move(view), .op = target_op::discard};
}

depth_stencil_target clear(depth_stencil_view view, float depth, cc::u8 stencil)
{
    return {.view = cc::move(view), .op = target_op::clear, .clear_depth = depth, .clear_stencil = stencil};
}

depth_stencil_target keep(depth_stencil_view view)
{
    return {.view = cc::move(view), .op = target_op::keep};
}

depth_stencil_target discard(depth_stencil_view view)
{
    return {.view = cc::move(view), .op = target_op::discard};
}

rendering_scope::rendering_scope(command_list& cmd, rendering_info const& info) : _cmd(cmd)
{
    _cmd.raster_begin_rendering(info);
}

rendering_scope::~rendering_scope()
{
    _cmd.raster_end_rendering();
}

void command_list_raster_manual_scope::begin_rendering(rendering_info const& info)
{
    _cmd.raster_begin_rendering(info);
}

void command_list_raster_manual_scope::end_rendering()
{
    _cmd.raster_end_rendering();
}

rendering_scope command_list_raster_scope::render_to(rendering_info const& info)
{
    return rendering_scope(_cmd, info);
}
} // namespace sg
