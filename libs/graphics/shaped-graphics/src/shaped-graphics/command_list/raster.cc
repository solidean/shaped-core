#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/string/format.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/command_list/raster.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>

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

depth_stencil_target depth_stencil_view::cleared(float depth, u8 stencil) const&
{
    return {.view = *this, .op = target_op::clear, .clear_depth = depth, .clear_stencil = stencil};
}
depth_stencil_target depth_stencil_view::cleared(float depth, u8 stencil) &&
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

void command_list::open_rendering(rendering_info const& info)
{
    _rendering_target_set = info.target_set;

    _rendering_targets.clear();
    for (auto const& target : info.color_targets)
        _rendering_targets.push_back({.texture = target.view.texture().get(), .range = target.view.range()});
    if (info.depth_stencil_target.has_value())
    {
        auto const& view = info.depth_stencil_target.value().view;
        _rendering_targets.push_back({.texture = view.texture().get(), .range = view.range()});
    }

    auto formats = raster_target_formats();
    for (auto const& target : info.color_targets)
        formats.color.push_back(target.view.format());
    if (info.depth_stencil_target.has_value())
        formats.depth_stencil = info.depth_stencil_target.value().view.format();
    // Every target of a rendering has one sample count, so the first one says it.
    if (!info.color_targets.empty())
        formats.sample_count = info.color_targets[0].view.texture()->sample_count();
    else if (info.depth_stencil_target.has_value())
        formats.sample_count = info.depth_stencil_target.value().view.texture()->sample_count();
    _rendering_formats = formats;

    raster_begin_rendering(info);
}

void command_list::close_rendering()
{
    _rendering_target_set.clear();
    _rendering_targets.clear();
    _rendering_formats = {};
    // A rendering scope binds nothing the next one inherits.
    _raster_layout = nullptr;
    for (auto& g : _raster_groups)
        g = nullptr;
    for (auto& vb : _vertex_buffers)
        vb = nullptr;
    _index_buffer = nullptr;
    raster_end_rendering();
}

void command_list::check_copy_outside_rendering_targets(raw_texture const* texture,
                                                        subresource_index const& subresource,
                                                        cc::string_view what) const
{
    for (auto const& target : _rendering_targets)
    {
        if (target.texture != texture)
            continue;
        // Only the subresources the target actually renders to are refused, so a copy into another mip or slice of
        // the same texture stays legal.
        // The aspect axis is left out on purpose: a depth-stencil target holds both planes in one layout, and the two
        // aspect numberings in play here disagree for depth formats.
        if (subresource.mip_level < target.range.mip_range.start || subresource.mip_level >= target.range.mip_range.end)
            continue;
        if (subresource.array_layer < target.range.array_range.start
            || subresource.array_layer >= target.range.array_range.end)
            continue;

        CC_ASSERTF(false,
                   "{} names mip {}, slice {} of a texture the open rendering '{}' draws into. A backend reopens the "
                   "suspended pass with the layouts its targets had, so nothing transitions the texture back out of "
                   "the copy's. Record the copy before render_to, or after the scope closes",
                   what, subresource.mip_level, subresource.array_layer, _rendering_target_set);
    }
}

void command_list::bind_raster_group(int group_index, binding_group const& group)
{
    CC_ASSERT(group_index >= 0 && group_index < max_binding_groups, "binding-group slot out of range");
    _raster_groups[group_index] = &group;
    raster_bind_group(group_index, group);
}

void command_list::bind_raster_vertex_buffers(int first_slot, cc::span<vertex_buffer_view const> views)
{
    CC_ASSERT(first_slot >= 0 && first_slot + views.size() <= max_vertex_buffers, "vertex-buffer slot out of range");
    for (auto i = isize(0); i < views.size(); ++i)
        _vertex_buffers[first_slot + i] = views[i].buffer.get();
    raster_bind_vertex_buffers(first_slot, views);
}

void command_list::bind_raster_index_buffer(index_buffer_view const& view)
{
    _index_buffer = view.buffer.get();
    raster_bind_index_buffer(view);
}

void command_list::check_raster_aliasing(bool indexed)
{
    if (context().portability_checks())
    {
        _raster_aliasing.clear();
        _raster_aliasing.add(_raster_groups, _vertex_buffers, indexed ? _index_buffer : nullptr);
    }
}

void command_list::draw(draw_config const& config)
{
    _stats.add(stat::draws);
    check_raster_aliasing(false);
    raster_draw(config);
}

void command_list::draw_indexed(draw_indexed_config const& config)
{
    _stats.add(stat::draws);
    check_raster_aliasing(true);
    raster_draw_indexed(config);
}

void command_list::bind_raster_pipeline(raster_pipeline const& pipeline)
{
    // A name empty on either side is a hand-built description, which the formats below still check.
    CC_ASSERTF(
        pipeline.target_set().empty() || _rendering_target_set.empty() || pipeline.target_set() == _rendering_target_set,
        "this pipeline draws into '{}', and the open rendering is '{}'", pipeline.target_set(), _rendering_target_set);

    // Backends bake the formats into the PSO, and none of them checks a rendering against it.
    if (pipeline.target_formats().has_value() && _rendering_formats.has_value())
    {
        auto const& built = pipeline.target_formats().value();
        auto const& open = _rendering_formats.value();
        CC_ASSERTF(built.color.size() == open.color.size(),
                   "this pipeline writes {} color targets, and the open rendering has {}", built.color.size(),
                   open.color.size());
        for (auto i = 0; i < built.color.size(); ++i)
            CC_ASSERTF(built.color[i] == open.color[i],
                       "color target {} of this pipeline is pixel_format {}, and the open rendering's is {}", i,
                       int(built.color[i]), int(open.color[i]));
        CC_ASSERTF(built.depth_stencil == open.depth_stencil,
                   "this pipeline's depth-stencil pixel_format is {}, and the open rendering's is {}",
                   int(built.depth_stencil), int(open.depth_stencil));
        CC_ASSERTF(built.sample_count == open.sample_count,
                   "this pipeline is built for {} samples, and the open rendering has {}", built.sample_count,
                   open.sample_count);
    }
    if (auto const* layout = pipeline.footprint().layout(); layout == nullptr || layout != _raster_layout)
    {
        _raster_layout = layout;
        for (auto& g : _raster_groups)
            g = nullptr;
    }
    raster_bind_pipeline(pipeline);
}

rendering_scope::rendering_scope(class command_list& cmd, rendering_info const& info) : _cmd(cmd)
{
    // Snapshot the target formats and extent so a routine recording into the scope reads them back rather than being told them again.
    // All targets in a scope share the extent; take it from the first color target, or the depth-stencil one for a depth-only pass.
    for (auto const& ct : info.color_targets)
        _color_formats.push_back(ct.view.format());

    if (info.depth_stencil_target.has_value())
        _depth_format = info.depth_stencil_target.value().view.format();

    if (!info.color_targets.empty())
        _size = info.color_targets.front().view.size();
    else if (info.depth_stencil_target.has_value())
        _size = info.depth_stencil_target.value().view.size();

    _cmd.open_rendering(info);
}

rendering_scope::~rendering_scope()
{
    _cmd.close_rendering();
}

// Raster draw recording on the scope — the same thin forwarders as command_list_raster_scope.
// They reach the command list's backend seams directly, since rendering_scope is a friend of command_list.

void rendering_scope::bind_pipeline(raster_pipeline const& pipeline)
{
    _cmd.bind_raster_pipeline(pipeline);
}
void rendering_scope::bind_group(int group_index, binding_group const& group)
{
    _cmd.bind_raster_group(group_index, group);
}
void rendering_scope::bind_vertex_buffers(cc::span<vertex_buffer_view const> views, int first_slot)
{
    _cmd.bind_raster_vertex_buffers(first_slot, views);
}
void rendering_scope::bind_vertex_buffers(std::initializer_list<vertex_buffer_view> views, int first_slot)
{
    _cmd.bind_raster_vertex_buffers(first_slot, cc::span<vertex_buffer_view const>(views.begin(), isize(views.size())));
}
void rendering_scope::bind_vertex_buffer(vertex_buffer_view const& view, int slot)
{
    _cmd.bind_raster_vertex_buffers(slot, cc::span<vertex_buffer_view const>(&view, 1));
}
void rendering_scope::bind_index_buffer(index_buffer_view const& view)
{
    _cmd.bind_raster_index_buffer(view);
}
void rendering_scope::declare_array_buffer_access(cc::string_view binding_name,
                                                  cc::span<array_buffer_access const> elements)
{
    _cmd.raster_declare_array_buffer_access(binding_name, elements);
}
void rendering_scope::declare_array_texture_access(cc::string_view binding_name,
                                                   cc::span<array_texture_access const> elements)
{
    _cmd.raster_declare_array_texture_access(binding_name, elements);
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
void rendering_scope::set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset)
{
    _cmd.raster_set_inline_constants(data, offset);
}
void rendering_scope::draw(draw_config const& config)
{
    _cmd.draw(config);
}
void rendering_scope::draw_indexed(draw_indexed_config const& config)
{
    _cmd.draw_indexed(config);
}

void command_list_raster_manual_scope::begin_rendering(rendering_info const& info)
{
    _cmd.open_rendering(info);
}

void command_list_raster_manual_scope::end_rendering()
{
    _cmd.close_rendering();
}

rendering_scope command_list_raster_scope::render_to(rendering_info const& info)
{
    return rendering_scope(_cmd, info);
}

// Draw recording — identical thin forwarders on both raster facades, cmd.raster and cmd.raster.manual.
// Each delegates to the shared command_list backend seams.

void command_list_raster_scope::bind_pipeline(raster_pipeline const& pipeline)
{
    _cmd.bind_raster_pipeline(pipeline);
}
void command_list_raster_scope::bind_group(int group_index, binding_group const& group)
{
    _cmd.bind_raster_group(group_index, group);
}
void command_list_raster_scope::bind_vertex_buffers(cc::span<vertex_buffer_view const> views, int first_slot)
{
    _cmd.bind_raster_vertex_buffers(first_slot, views);
}
void command_list_raster_scope::bind_vertex_buffers(std::initializer_list<vertex_buffer_view> views, int first_slot)
{
    _cmd.bind_raster_vertex_buffers(first_slot, cc::span<vertex_buffer_view const>(views.begin(), isize(views.size())));
}
void command_list_raster_scope::bind_vertex_buffer(vertex_buffer_view const& view, int slot)
{
    _cmd.bind_raster_vertex_buffers(slot, cc::span<vertex_buffer_view const>(&view, 1));
}
void command_list_raster_scope::bind_index_buffer(index_buffer_view const& view)
{
    _cmd.bind_raster_index_buffer(view);
}
void command_list_raster_scope::declare_array_buffer_access(cc::string_view binding_name,
                                                            cc::span<array_buffer_access const> elements)
{
    _cmd.raster_declare_array_buffer_access(binding_name, elements);
}
void command_list_raster_scope::declare_array_texture_access(cc::string_view binding_name,
                                                             cc::span<array_texture_access const> elements)
{
    _cmd.raster_declare_array_texture_access(binding_name, elements);
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
void command_list_raster_scope::set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset)
{
    _cmd.raster_set_inline_constants(data, offset);
}
void command_list_raster_scope::draw(draw_config const& config)
{
    _cmd.draw(config);
}
void command_list_raster_scope::draw_indexed(draw_indexed_config const& config)
{
    _cmd.draw_indexed(config);
}

void command_list_raster_manual_scope::bind_pipeline(raster_pipeline const& pipeline)
{
    _cmd.bind_raster_pipeline(pipeline);
}
void command_list_raster_manual_scope::bind_group(int group_index, binding_group const& group)
{
    _cmd.bind_raster_group(group_index, group);
}
void command_list_raster_manual_scope::bind_vertex_buffers(cc::span<vertex_buffer_view const> views, int first_slot)
{
    _cmd.bind_raster_vertex_buffers(first_slot, views);
}
void command_list_raster_manual_scope::bind_vertex_buffers(std::initializer_list<vertex_buffer_view> views, int first_slot)
{
    _cmd.bind_raster_vertex_buffers(first_slot, cc::span<vertex_buffer_view const>(views.begin(), isize(views.size())));
}
void command_list_raster_manual_scope::bind_vertex_buffer(vertex_buffer_view const& view, int slot)
{
    _cmd.bind_raster_vertex_buffers(slot, cc::span<vertex_buffer_view const>(&view, 1));
}
void command_list_raster_manual_scope::bind_index_buffer(index_buffer_view const& view)
{
    _cmd.bind_raster_index_buffer(view);
}
void command_list_raster_manual_scope::declare_array_buffer_access(cc::string_view binding_name,
                                                                   cc::span<array_buffer_access const> elements)
{
    _cmd.raster_declare_array_buffer_access(binding_name, elements);
}
void command_list_raster_manual_scope::declare_array_texture_access(cc::string_view binding_name,
                                                                    cc::span<array_texture_access const> elements)
{
    _cmd.raster_declare_array_texture_access(binding_name, elements);
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
void command_list_raster_manual_scope::set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset)
{
    _cmd.raster_set_inline_constants(data, offset);
}
void command_list_raster_manual_scope::draw(draw_config const& config)
{
    _cmd.draw(config);
}
void command_list_raster_manual_scope::draw_indexed(draw_indexed_config const& config)
{
    _cmd.draw_indexed(config);
}
} // namespace sg
