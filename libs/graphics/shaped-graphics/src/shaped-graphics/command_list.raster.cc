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

rendering_scope::rendering_scope(class command_list& cmd, rendering_info const& info) : _cmd(cmd)
{
    // Snapshot the target formats and extent so a routine recording into the scope reads them back rather
    // than being told them again. All targets in a scope share the extent; take it from the first color
    // target, or the depth-stencil one for a depth-only pass.
    for (auto const& ct : info.color_targets)
        _color_formats.push_back(ct.view.format());

    if (info.depth_stencil_target.has_value())
        _depth_format = info.depth_stencil_target.value().view.format();

    if (!info.color_targets.empty())
        _size = info.color_targets.front().view.size();
    else if (info.depth_stencil_target.has_value())
        _size = info.depth_stencil_target.value().view.size();

    _cmd.raster_begin_rendering(info);
}

rendering_scope::~rendering_scope()
{
    _cmd.raster_end_rendering();
}

// Raster draw recording on the scope — the same thin forwarders as command_list_raster_scope, reaching the
// command list's backend seams directly (rendering_scope is a friend of command_list).

void rendering_scope::bind_pipeline(raster_pipeline const& pipeline)
{
    _cmd.raster_bind_pipeline(pipeline);
}
void rendering_scope::bind_group(int set, binding_group const& group)
{
    _cmd.raster_bind_group(set, group);
}
void rendering_scope::bind_vertex_buffers(cc::span<vertex_buffer_view const> views, int first_slot)
{
    _cmd.raster_bind_vertex_buffers(first_slot, views);
}
void rendering_scope::bind_vertex_buffers(std::initializer_list<vertex_buffer_view> views, int first_slot)
{
    _cmd.raster_bind_vertex_buffers(first_slot,
                                    cc::span<vertex_buffer_view const>(views.begin(), cc::isize(views.size())));
}
void rendering_scope::bind_vertex_buffer(vertex_buffer_view const& view, int slot)
{
    _cmd.raster_bind_vertex_buffers(slot, cc::span<vertex_buffer_view const>(&view, 1));
}
void rendering_scope::bind_index_buffer(index_buffer_view const& view)
{
    _cmd.raster_bind_index_buffer(view);
}
void rendering_scope::set_viewport(viewport const& vp)
{
    _cmd.raster_set_viewport(vp);
}
void rendering_scope::set_scissor(tg::aabb2i const& rect)
{
    _cmd.raster_set_scissor(rect);
}
void rendering_scope::set_stencil_reference(u32 reference)
{
    _cmd.raster_set_stencil_reference(reference);
}
void rendering_scope::set_blend_constants(tg::vec4f constants)
{
    _cmd.raster_set_blend_constants(constants);
}
void rendering_scope::set_inline_constants(cc::span<cc::byte const> data, cc::optional<cc::isize> offset)
{
    _cmd.raster_set_inline_constants(data, offset);
}
void rendering_scope::draw(draw_config const& config)
{
    _cmd.raster_draw(config);
}
void rendering_scope::draw_indexed(draw_indexed_config const& config)
{
    _cmd.raster_draw_indexed(config);
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

// Draw recording — identical thin forwarders on both raster facades (cmd.raster and cmd.raster.manual),
// each delegating to the shared command_list backend seams.

void command_list_raster_scope::bind_pipeline(raster_pipeline const& pipeline)
{
    _cmd.raster_bind_pipeline(pipeline);
}
void command_list_raster_scope::bind_group(int set, binding_group const& group)
{
    _cmd.raster_bind_group(set, group);
}
void command_list_raster_scope::bind_vertex_buffers(cc::span<vertex_buffer_view const> views, int first_slot)
{
    _cmd.raster_bind_vertex_buffers(first_slot, views);
}
void command_list_raster_scope::bind_vertex_buffers(std::initializer_list<vertex_buffer_view> views, int first_slot)
{
    _cmd.raster_bind_vertex_buffers(first_slot,
                                    cc::span<vertex_buffer_view const>(views.begin(), cc::isize(views.size())));
}
void command_list_raster_scope::bind_vertex_buffer(vertex_buffer_view const& view, int slot)
{
    _cmd.raster_bind_vertex_buffers(slot, cc::span<vertex_buffer_view const>(&view, 1));
}
void command_list_raster_scope::bind_index_buffer(index_buffer_view const& view)
{
    _cmd.raster_bind_index_buffer(view);
}
void command_list_raster_scope::set_viewport(viewport const& vp)
{
    _cmd.raster_set_viewport(vp);
}
void command_list_raster_scope::set_scissor(tg::aabb2i const& rect)
{
    _cmd.raster_set_scissor(rect);
}
void command_list_raster_scope::set_stencil_reference(u32 reference)
{
    _cmd.raster_set_stencil_reference(reference);
}
void command_list_raster_scope::set_blend_constants(tg::vec4f constants)
{
    _cmd.raster_set_blend_constants(constants);
}
void command_list_raster_scope::set_inline_constants(cc::span<cc::byte const> data, cc::optional<cc::isize> offset)
{
    _cmd.raster_set_inline_constants(data, offset);
}
void command_list_raster_scope::draw(draw_config const& config)
{
    _cmd.raster_draw(config);
}
void command_list_raster_scope::draw_indexed(draw_indexed_config const& config)
{
    _cmd.raster_draw_indexed(config);
}

void command_list_raster_manual_scope::bind_pipeline(raster_pipeline const& pipeline)
{
    _cmd.raster_bind_pipeline(pipeline);
}
void command_list_raster_manual_scope::bind_group(int set, binding_group const& group)
{
    _cmd.raster_bind_group(set, group);
}
void command_list_raster_manual_scope::bind_vertex_buffers(cc::span<vertex_buffer_view const> views, int first_slot)
{
    _cmd.raster_bind_vertex_buffers(first_slot, views);
}
void command_list_raster_manual_scope::bind_vertex_buffers(std::initializer_list<vertex_buffer_view> views, int first_slot)
{
    _cmd.raster_bind_vertex_buffers(first_slot,
                                    cc::span<vertex_buffer_view const>(views.begin(), cc::isize(views.size())));
}
void command_list_raster_manual_scope::bind_vertex_buffer(vertex_buffer_view const& view, int slot)
{
    _cmd.raster_bind_vertex_buffers(slot, cc::span<vertex_buffer_view const>(&view, 1));
}
void command_list_raster_manual_scope::bind_index_buffer(index_buffer_view const& view)
{
    _cmd.raster_bind_index_buffer(view);
}
void command_list_raster_manual_scope::set_viewport(viewport const& vp)
{
    _cmd.raster_set_viewport(vp);
}
void command_list_raster_manual_scope::set_scissor(tg::aabb2i const& rect)
{
    _cmd.raster_set_scissor(rect);
}
void command_list_raster_manual_scope::set_stencil_reference(u32 reference)
{
    _cmd.raster_set_stencil_reference(reference);
}
void command_list_raster_manual_scope::set_blend_constants(tg::vec4f constants)
{
    _cmd.raster_set_blend_constants(constants);
}
void command_list_raster_manual_scope::set_inline_constants(cc::span<cc::byte const> data, cc::optional<cc::isize> offset)
{
    _cmd.raster_set_inline_constants(data, offset);
}
void command_list_raster_manual_scope::draw(draw_config const& config)
{
    _cmd.raster_draw(config);
}
void command_list_raster_manual_scope::draw_indexed(draw_indexed_config const& config)
{
    _cmd.raster_draw_indexed(config);
}
} // namespace sg
