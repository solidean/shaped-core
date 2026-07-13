#include <clean-core/common/utility.hh> // cc::move
#include <shaped-graphics/command_list.hh>
#include <shaped-graphics/command_list.raster.hh>

namespace sg
{
// Color-target builders — the op is the method name, so no op enum appears at the call site.

color_target render_target_view::cleared(tg::vec4f color) const&
{
    return {.view = *this, .op = target_op::clear, .clear_color = color};
}
color_target render_target_view::cleared(tg::vec4f color) &&
{
    return {.view = cc::move(*this), .op = target_op::clear, .clear_color = color};
}

color_target render_target_view::preserved() const&
{
    return {.view = *this, .op = target_op::preserve};
}
color_target render_target_view::preserved() &&
{
    return {.view = cc::move(*this), .op = target_op::preserve};
}

color_target render_target_view::discarded() const&
{
    return {.view = *this, .op = target_op::discard};
}
color_target render_target_view::discarded() &&
{
    return {.view = cc::move(*this), .op = target_op::discard};
}

// Depth-stencil-target builders.

depth_stencil_target depth_stencil_view::cleared(float depth, cc::u8 stencil) const&
{
    return {.view = *this, .op = target_op::clear, .clear_depth = depth, .clear_stencil = stencil};
}
depth_stencil_target depth_stencil_view::cleared(float depth, cc::u8 stencil) &&
{
    return {.view = cc::move(*this), .op = target_op::clear, .clear_depth = depth, .clear_stencil = stencil};
}

depth_stencil_target depth_stencil_view::preserved() const&
{
    return {.view = *this, .op = target_op::preserve};
}
depth_stencil_target depth_stencil_view::preserved() &&
{
    return {.view = cc::move(*this), .op = target_op::preserve};
}

depth_stencil_target depth_stencil_view::discarded() const&
{
    return {.view = *this, .op = target_op::discard};
}
depth_stencil_target depth_stencil_view::discarded() &&
{
    return {.view = cc::move(*this), .op = target_op::discard};
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
