// Raster recording for the webgpu backend: the rendering scope, its reopen around copies, and draws.

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>
#include <shaped-graphics/backends/webgpu/webgpu_format.hh>

namespace sg::backend::webgpu
{
void webgpu_command_list::raster_begin_rendering(sg::rendering_info const& info)
{
    CC_ASSERT(!_in_rendering_scope, "a rendering scope is already open");
    CC_ASSERT(!info.color_targets.empty() || info.depth_stencil_target.has_value(), "a rendering scope needs at least "
                                                                                    "one target");
    end_open_pass();

    _color_attachments.clear();
    auto size = tg::vec2i(0, 0);
    for (auto const& target : info.color_targets)
    {
        auto const texture = std::dynamic_pointer_cast<webgpu_texture const>(target.view.texture());
        CC_ASSERT(texture != nullptr, "render target is not a webgpu texture");
        CC_ASSERT(!texture->is_expired(), "render target is a transient texture used past its epoch (expired)");
        touch(target.view.texture());

        auto attachment = color_attachment();
        attachment.view = texture->create_view(target.view.dimension(), target.view.format(), target.view.range());
        attachment.attachment.view = attachment.view.get();
        attachment.attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        attachment.attachment.resolveTarget = nullptr;
        // WebGPU has no "don't care" load, and clearing is the cheap way to not load.
        attachment.attachment.loadOp = target.op == sg::target_op::preserve ? WGPULoadOp_Load : WGPULoadOp_Clear;
        attachment.attachment.storeOp = WGPUStoreOp_Store;
        auto const c = target.op == sg::target_op::clear ? target.clear_color : tg::vec4f(0, 0, 0, 0);
        attachment.attachment.clearValue = WGPUColor{c[0], c[1], c[2], c[3]};
        _color_attachments.push_back(cc::move(attachment));
        size = target.view.size();
    }

    _has_depth = info.depth_stencil_target.has_value();
    _depth_view = {};
    if (_has_depth)
    {
        auto const& target = info.depth_stencil_target.value();
        auto const texture = std::dynamic_pointer_cast<webgpu_texture const>(target.view.texture());
        CC_ASSERT(texture != nullptr, "depth-stencil target is not a webgpu texture");
        CC_ASSERT(!texture->is_expired(), "depth-stencil target is a transient texture used past its epoch (expired)");
        touch(target.view.texture());

        // Whole-format view: a depth-stencil attachment names both planes, and an aspect-restricted view is refused.
        auto range = target.view.range();
        range.aspect_range = {.start = 0, .end = sg::format_aspect_count(texture->format())};
        _depth_view = texture->create_view(target.view.dimension(), target.view.format(), range);

        auto const load = target.op == sg::target_op::preserve ? WGPULoadOp_Load : WGPULoadOp_Clear;
        _depth_attachment = WGPURenderPassDepthStencilAttachment{};
        _depth_attachment.view = _depth_view.get();
        _depth_attachment.depthLoadOp = load;
        _depth_attachment.depthStoreOp = WGPUStoreOp_Store;
        _depth_attachment.depthClearValue = target.op == sg::target_op::clear ? target.clear_depth : 1.0f;
        _depth_attachment.depthReadOnly = WGPU_FALSE;
        if (sg::has_stencil(texture->format()))
        {
            _depth_attachment.stencilLoadOp = load;
            _depth_attachment.stencilStoreOp = WGPUStoreOp_Store;
            _depth_attachment.stencilClearValue = target.op == sg::target_op::clear ? target.clear_stencil : 0;
        }
        else
        {
            _depth_attachment.stencilLoadOp = WGPULoadOp_Undefined;
            _depth_attachment.stencilStoreOp = WGPUStoreOp_Undefined;
        }
        if (info.color_targets.empty())
            size = target.view.size();
    }

    _target_size = size;
    if (info.viewport.has_value())
    {
        auto const& vp = info.viewport.value();
        _viewport = {vp.offset[0], vp.offset[1], vp.size[0], vp.size[1], vp.min_depth, vp.max_depth};
    }
    else
        _viewport = {0, 0, float(size[0]), float(size[1]), 0, 1};
    _scissor = info.scissor.has_value() ? info.scissor.value() : tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(size[0], size[1]));
    _stencil_reference = 0;
    _blend_constants = WGPUColor{0, 0, 0, 0};
    _vertex_buffers.clear();
    _index_buffer = nullptr;
    _raster = bound_state();

    _in_rendering_scope = true;

    // Opened at once rather than at the first draw, so a scope that only clears still clears.
    open_render_pass(false);
}

void webgpu_command_list::open_render_pass(bool reopen)
{
    CC_ASSERT(_in_rendering_scope, "no rendering scope to open a pass for");
    if (_compute_pass)
        end_open_pass();

    if (reopen)
    {
        for (auto& a : _color_attachments)
            a.attachment.loadOp = WGPULoadOp_Load;
        if (_has_depth)
        {
            _depth_attachment.depthLoadOp = WGPULoadOp_Load;
            if (_depth_attachment.stencilLoadOp != WGPULoadOp_Undefined)
                _depth_attachment.stencilLoadOp = WGPULoadOp_Load;
        }
    }

    auto attachments = cc::fixed_vector<WGPURenderPassColorAttachment, sg::max_color_targets>();
    for (auto const& a : _color_attachments)
        attachments.push_back(a.attachment);
    auto const desc = WGPURenderPassDescriptor{
        .nextInChain = nullptr,
        .label = to_wgpu("sg rendering scope"),
        .colorAttachmentCount = size_t(attachments.size()),
        .colorAttachments = attachments.empty() ? nullptr : attachments.data(),
        .depthStencilAttachment = _has_depth ? &_depth_attachment : nullptr,
        .occlusionQuerySet = nullptr,
        .timestampWrites = nullptr,
    };
    _render_pass = wgpu_render_pass(wgpuCommandEncoderBeginRenderPass(_encoder.get(), &desc));

    auto const pass = _render_pass.get();
    wgpuRenderPassEncoderSetViewport(pass, _viewport.x, _viewport.y, _viewport.width, _viewport.height,
                                     _viewport.min_depth, _viewport.max_depth);
    auto const x0 = _scissor.min[0] < 0 ? 0 : _scissor.min[0];
    auto const y0 = _scissor.min[1] < 0 ? 0 : _scissor.min[1];
    auto const x1 = _scissor.max[0] > _target_size[0] ? _target_size[0] : _scissor.max[0];
    auto const y1 = _scissor.max[1] > _target_size[1] ? _target_size[1] : _scissor.max[1];
    wgpuRenderPassEncoderSetScissorRect(pass, u32(x0), u32(y0), u32(x1 > x0 ? x1 - x0 : 0), u32(y1 > y0 ? y1 - y0 : 0));
    wgpuRenderPassEncoderSetStencilReference(pass, _stencil_reference);
    wgpuRenderPassEncoderSetBlendConstant(pass, &_blend_constants);
    for (isize i = 0; i < _vertex_buffers.size(); ++i)
        if (_vertex_buffers[i].buffer != nullptr)
            wgpuRenderPassEncoderSetVertexBuffer(pass, u32(i), _vertex_buffers[i].buffer, _vertex_buffers[i].offset,
                                                 _vertex_buffers[i].size);
    if (_index_buffer != nullptr)
        wgpuRenderPassEncoderSetIndexBuffer(pass, _index_buffer, _index_format, _index_offset, _index_size);
    _raster.needs_full_apply = true;
}

void webgpu_command_list::raster_end_rendering()
{
    CC_ASSERT(_in_rendering_scope, "end_rendering without an open rendering scope");
    if (_render_pass)
        end_open_pass();
    _in_rendering_scope = false;
    _color_attachments.clear();
    _depth_view = {};
    _has_depth = false;
    _vertex_buffers.clear();
    _index_buffer = nullptr;
    _raster = bound_state();
}

void webgpu_command_list::apply_raster_state()
{
    auto& s = _raster;
    CC_ASSERT(s.render_pipeline != nullptr, "bind a raster pipeline before drawing");
    if (!_render_pass)
        open_render_pass(true);

    place_constants(s);
    if (!s.needs_full_apply)
        return;

    auto const pass = _render_pass.get();
    wgpuRenderPassEncoderSetPipeline(pass, s.render_pipeline);
    for (isize i = 0; i < s.groups.size(); ++i)
        if (s.groups[i] != nullptr)
            wgpuRenderPassEncoderSetBindGroup(pass, u32(i), s.groups[i], 0, nullptr);
    if (s.layout->has_reserved_group())
    {
        for (auto i = s.groups.size(); i < sg::reserved_binding_group; ++i)
            wgpuRenderPassEncoderSetBindGroup(pass, u32(i), s.layout->empty_group(), 0, nullptr);
        auto const has_constants = s.layout->inline_constants_bytes() > 0;
        auto const offset = s.constants_offset;
        wgpuRenderPassEncoderSetBindGroup(pass, u32(sg::reserved_binding_group),
                                          s.layout->reserved_group_for(s.constants_page), has_constants ? 1 : 0,
                                          has_constants ? &offset : nullptr);
    }
    s.needs_full_apply = false;
}

void webgpu_command_list::raster_bind_vertex_buffers(int first_slot, cc::span<sg::vertex_buffer_view const> views)
{
    CC_ASSERT(_in_rendering_scope, "bind_vertex_buffers is only valid inside a rendering scope");
    CC_ASSERT(first_slot >= 0 && first_slot + views.size() <= sg::max_vertex_buffers, "vertex buffer slot out of "
                                                                                      "range");
    while (_vertex_buffers.size() < first_slot + views.size())
        _vertex_buffers.push_back({});
    for (isize i = 0; i < views.size(); ++i)
    {
        auto const& v = views[i];
        auto const* buffer = dynamic_cast<webgpu_buffer const*>(v.buffer.get());
        CC_ASSERT(buffer != nullptr, "vertex buffer is not a webgpu buffer");
        touch(v.buffer);
        auto& binding = _vertex_buffers[first_slot + i];
        binding = {.buffer = buffer->raw(), .offset = u64(v.offset_in_bytes), .size = u64(v.size_in_bytes)};
        if (_render_pass && binding.size > 0)
            wgpuRenderPassEncoderSetVertexBuffer(_render_pass.get(), u32(first_slot + i), binding.buffer,
                                                 binding.offset, binding.size);
    }
}

void webgpu_command_list::raster_bind_index_buffer(sg::index_buffer_view const& view)
{
    CC_ASSERT(_in_rendering_scope, "bind_index_buffer is only valid inside a rendering scope");
    auto const* buffer = dynamic_cast<webgpu_buffer const*>(view.buffer.get());
    CC_ASSERT(buffer != nullptr, "index buffer is not a webgpu buffer");
    touch(view.buffer);
    _index_buffer = buffer->raw();
    _index_format = view.format == sg::index_format::uint16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
    _index_offset = u64(view.offset_in_bytes);
    _index_size = u64(view.size_in_bytes);
    if (_render_pass)
        wgpuRenderPassEncoderSetIndexBuffer(_render_pass.get(), _index_buffer, _index_format, _index_offset, _index_size);
}

void webgpu_command_list::raster_set_viewport(sg::viewport const& vp)
{
    CC_ASSERT(_in_rendering_scope, "set_viewport is only valid inside a rendering scope");
    _viewport = {vp.offset[0], vp.offset[1], vp.size[0], vp.size[1], vp.min_depth, vp.max_depth};
    if (_render_pass)
        wgpuRenderPassEncoderSetViewport(_render_pass.get(), _viewport.x, _viewport.y, _viewport.width,
                                         _viewport.height, _viewport.min_depth, _viewport.max_depth);
}

void webgpu_command_list::raster_set_scissor(tg::aabb2i const& rect)
{
    CC_ASSERT(_in_rendering_scope, "set_scissor is only valid inside a rendering scope");
    _scissor = rect;
    if (_render_pass)
    {
        auto const x0 = rect.min[0] < 0 ? 0 : rect.min[0];
        auto const y0 = rect.min[1] < 0 ? 0 : rect.min[1];
        auto const x1 = rect.max[0] > _target_size[0] ? _target_size[0] : rect.max[0];
        auto const y1 = rect.max[1] > _target_size[1] ? _target_size[1] : rect.max[1];
        wgpuRenderPassEncoderSetScissorRect(_render_pass.get(), u32(x0), u32(y0), u32(x1 > x0 ? x1 - x0 : 0),
                                            u32(y1 > y0 ? y1 - y0 : 0));
    }
}

void webgpu_command_list::raster_set_stencil_reference(u32 reference)
{
    CC_ASSERT(_in_rendering_scope, "set_stencil_reference is only valid inside a rendering scope");
    _stencil_reference = reference;
    if (_render_pass)
        wgpuRenderPassEncoderSetStencilReference(_render_pass.get(), reference);
}

void webgpu_command_list::raster_set_blend_constants(tg::vec4f constants)
{
    CC_ASSERT(_in_rendering_scope, "set_blend_constants is only valid inside a rendering scope");
    _blend_constants = WGPUColor{constants[0], constants[1], constants[2], constants[3]};
    if (_render_pass)
        wgpuRenderPassEncoderSetBlendConstant(_render_pass.get(), &_blend_constants);
}

void webgpu_command_list::raster_draw(sg::draw_config const& config)
{
    CC_ASSERT(_in_rendering_scope, "draw is only valid inside a rendering scope");
    apply_raster_state();
    wgpuRenderPassEncoderDraw(_render_pass.get(), u32(config.vertex_range.size), u32(config.instance_range.size),
                              u32(config.vertex_range.offset), u32(config.instance_range.offset));
}

void webgpu_command_list::raster_draw_indexed(sg::draw_indexed_config const& config)
{
    CC_ASSERT(_in_rendering_scope, "draw_indexed is only valid inside a rendering scope");
    CC_ASSERT(_index_buffer != nullptr, "draw_indexed needs a bound index buffer");
    apply_raster_state();
    wgpuRenderPassEncoderDrawIndexed(_render_pass.get(), u32(config.index_range.size), u32(config.instance_range.size),
                                     u32(config.index_range.offset), config.vertex_offset,
                                     u32(config.instance_range.offset));
}
} // namespace sg::backend::webgpu
