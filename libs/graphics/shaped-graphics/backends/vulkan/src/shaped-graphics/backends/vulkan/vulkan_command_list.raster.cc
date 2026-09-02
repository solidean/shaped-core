// vulkan_command_list raster recording.
// The rendering scope transitions its targets, resolves their image views and opens a dynamic-rendering instance;
// the bind path and draws follow the compute ones, keyed to the graphics stages.
//
// Two things differ from dx12 structurally.
// There is no render-pass or framebuffer object to build and no RTV/DSV heap to allocate from, because dynamic
// rendering takes image views directly — so `sg::rendering_info` maps almost field for field onto
// VkRenderingAttachmentInfo, clear values and all.
// And a target's begin-op is the attachment's loadOp rather than a separate clear command, which is what lets a tiler
// avoid loading contents it is about to overwrite.

#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/container/fixed_vector.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_command_list.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_format.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raster_pipeline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>
#include <shaped-graphics/barrier/access_inference.hh>
#include <shaped-graphics/command_list/raster.hh>
#include <shaped-graphics/resource/pixel_format.hh>

namespace sg::backend::vulkan
{
namespace
{
[[nodiscard]] vulkan_texture const& as_vulkan_texture(sg::raw_texture_handle const& tex)
{
    // Only vulkan textures ever reach a vulkan command list, so static_cast is sound.
    // The dynamic_cast is the debug-only check, stripped in release where CC_ASSERT leaves its condition unevaluated.
    CC_ASSERT(std::dynamic_pointer_cast<vulkan_texture const>(tex) != nullptr, "target texture is not a vulkan "
                                                                               "texture");
    return static_cast<vulkan_texture const&>(*tex);
}

[[nodiscard]] vulkan_buffer const& as_vulkan_buffer(sg::raw_buffer_handle const& buf)
{
    CC_ASSERT(std::dynamic_pointer_cast<vulkan_buffer const>(buf) != nullptr, "buffer is not a vulkan buffer");
    return static_cast<vulkan_buffer const&>(*buf);
}

/// The attachment load operation a target's begin-op asks for.
/// `discard` is DONT_CARE, which is the point of having it: the contents become undefined rather than being loaded.
[[nodiscard]] VkAttachmentLoadOp to_vk_load_op(sg::target_op op)
{
    switch (op)
    {
    case sg::target_op::preserve:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case sg::target_op::clear:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case sg::target_op::discard:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    CC_UNREACHABLE("unhandled target_op");
}
} // namespace

void vulkan_command_list::raster_begin_rendering(sg::rendering_info const& info)
{
    CC_ASSERT(!_in_render_pass, "begin_rendering called while a rendering scope is already open");
    CC_ASSERT(!info.color_targets.empty() || info.depth_stencil_target.has_value(),
              "a rendering scope needs at least one color or depth-stencil target");

    // 1) Transition each target to its output layout and flush before the instance opens.
    //    A layout transition inside a dynamic-rendering instance is not allowed, so this cannot be deferred the way a
    //    dispatch's barriers are.
    for (auto const& ct : info.color_targets)
        (void)track_texture_access(as_vulkan_texture(ct.view.texture()), ct.view.range(),
                                   sg::pipeline_stage_flag::render_target, sg::access_flag::color_write,
                                   sg::texture_layout::render_target);
    if (info.depth_stencil_target.has_value())
    {
        auto const& dt = info.depth_stencil_target.value();
        (void)track_texture_access(as_vulkan_texture(dt.view.texture()), dt.view.range(),
                                   sg::pipeline_stage_flag::depth_stencil_target, sg::access_flag::depth_write,
                                   sg::texture_layout::depth_readwrite);
    }
    flush_barriers();

    // 2) One attachment per target, each naming a cached image view.
    //    Kept on the list, because a mid-pass barrier has to close and reopen the instance from them.
    auto& color_attachments = _rendering_color_attachments;
    color_attachments.clear();
    for (auto const& ct : info.color_targets)
    {
        auto attachment = VkRenderingAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = _ctx._image_views.acquire_attachment(ct.view.texture(), ct.view.dimension(), ct.view.format(),
                                                              ct.view.range()),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = to_vk_load_op(ct.op),
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
        if (ct.op == sg::target_op::clear)
            attachment.clearValue.color
                = {.float32 = {ct.clear_color[0], ct.clear_color[1], ct.clear_color[2], ct.clear_color[3]}};
        color_attachments.push_back(attachment);
    }

    auto& depth_attachment = _rendering_depth_attachment;
    auto& stencil_attachment = _rendering_stencil_attachment;
    depth_attachment = {};
    stencil_attachment = {};
    _rendering_has_depth = info.depth_stencil_target.has_value();
    _rendering_has_stencil = false;
    if (info.depth_stencil_target.has_value())
    {
        auto const& dt = info.depth_stencil_target.value();
        depth_attachment = VkRenderingAttachmentInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = _ctx._image_views.acquire_attachment(dt.view.texture(), dt.view.dimension(), dt.view.format(),
                                                              dt.view.range()),
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .loadOp = to_vk_load_op(dt.op),
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        };
        depth_attachment.clearValue.depthStencil = {.depth = dt.clear_depth, .stencil = u32(dt.clear_stencil)};

        // Vulkan takes the two aspects as separate attachments, where sg (like D3D12) has one depth-stencil target.
        // They name the same view and the same op — the split is the API's, not the caller's.
        _rendering_has_stencil = sg::has_stencil(dt.view.format());
        stencil_attachment = depth_attachment;
    }

    // 3) The render area, and the viewport / scissor defaults, both from the target extent.
    int extent_w = 1;
    int extent_h = 1;
    if (!info.color_targets.empty())
    {
        extent_w = info.color_targets[0].view.width();
        extent_h = info.color_targets[0].view.height();
    }
    else
    {
        extent_w = info.depth_stencil_target.value().view.width();
        extent_h = info.depth_stencil_target.value().view.height();
    }

    _rendering_area = {.offset = {0, 0}, .extent = {u32(extent_w), u32(extent_h)}};

    auto const rendering = VkRenderingInfo{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = _rendering_area,
        .layerCount = 1,
        .colorAttachmentCount = u32(color_attachments.size()),
        .pColorAttachments = color_attachments.empty() ? nullptr : color_attachments.data(),
        .pDepthAttachment = _rendering_has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = _rendering_has_stencil ? &stencil_attachment : nullptr,
    };
    vkCmdBeginRendering(_buffer, &rendering);
    _in_render_pass = true;

    // 4) Viewport + scissor default to the full target extent, the common "render to the whole target" case.
    if (info.viewport.has_value())
        raster_set_viewport(info.viewport.value());
    else
        raster_set_viewport({.offset = tg::pos2f(0.0f, 0.0f), .size = tg::vec2f(float(extent_w), float(extent_h))});

    if (info.scissor.has_value())
        raster_set_scissor(info.scissor.value());
    else
        raster_set_scissor(tg::aabb2i(tg::pos2i(0, 0), tg::pos2i(extent_w, extent_h)));
}

void vulkan_command_list::reopen_rendering()
{
    // Every load op becomes LOAD: the contents are real by now, and a CLEAR here would erase what the pass has
    // already drawn.
    // Bound pipeline, descriptors, vertex buffers and dynamic state are command-buffer scoped rather than instance
    // scoped, so none of them has to be replayed.
    for (auto& a : _rendering_color_attachments)
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    _rendering_depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    _rendering_stencil_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;

    auto const rendering = VkRenderingInfo{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = _rendering_area,
        .layerCount = 1,
        .colorAttachmentCount = u32(_rendering_color_attachments.size()),
        .pColorAttachments = _rendering_color_attachments.empty() ? nullptr : _rendering_color_attachments.data(),
        .pDepthAttachment = _rendering_has_depth ? &_rendering_depth_attachment : nullptr,
        .pStencilAttachment = _rendering_has_stencil ? &_rendering_stencil_attachment : nullptr,
    };
    vkCmdBeginRendering(_buffer, &rendering);
}

void vulkan_command_list::raster_end_rendering()
{
    CC_ASSERT(_in_render_pass, "end_rendering called with no open rendering scope");
    vkCmdEndRendering(_buffer);
    _in_render_pass = false;
    _rendering_color_attachments.clear();
    _rendering_has_depth = false;
    _rendering_has_stencil = false;

    // The graphics bind + IA state is scoped to the pass it was set up in.
    _bound_raster_layout = nullptr;
    _bound_raster_groups.clear();
    _bound_vertex_buffers.clear();
    _bound_index_buffer = nullptr;
}

// --- draw recording (reached through cmd.raster / cmd.raster.manual) -----------------------------------

void vulkan_command_list::raster_bind_pipeline(sg::raster_pipeline const& pipeline)
{
    CC_ASSERT(_in_render_pass, "bind_pipeline requires an open rendering scope");
    auto const* rp = dynamic_cast<vulkan_raster_pipeline const*>(&pipeline);
    CC_ASSERT(rp != nullptr, "raster_pipeline is not a vulkan raster_pipeline");

    auto const binding = VkDescriptorBufferBindingInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
        .address = _ctx._descriptor_heap.device_address(),
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
    };
    _ctx._descriptor_functions.cmd_bind_descriptor_buffers(_buffer, 1, &binding);
    vkCmdBindPipeline(_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, rp->_pipeline);

    // The topology is baked into the pipeline, so unlike dx12 there is no separate IA topology call.
    _bound_raster_layout = rp->layout.get();
    _bound_raster_groups.clear_resize_to_filled(_bound_raster_layout->_groups.size(), nullptr);
}

void vulkan_command_list::raster_bind_group(int group_index, sg::binding_group const& group)
{
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before binding groups");
    CC_ASSERT(group_index >= 0 && group_index < int(_bound_raster_groups.size()), "binding-group slot out of range "
                                                                                  "for the bound pipeline layout");

    auto const* vg = dynamic_cast<vulkan_binding_group const*>(&group);
    CC_ASSERT(vg != nullptr, "binding_group is not a vulkan binding_group");
    CC_ASSERT(!(vg->transient && vg->creation_epoch != _ctx.current_epoch()),
              "transient binding_group used past its epoch (its descriptors have been recycled)");
    CC_ASSERT(vg->layout == _bound_raster_layout->_groups[group_index], "binding_group's layout does not match the "
                                                                        "pipeline layout's slot");
    auto const pinned = vg->layout->group_index();
    CC_ASSERTF(!pinned.has_value() || pinned.value() == u32(group_index),
               "binding_group is pinned to group index {} by its bindings and cannot be bound at slot {}",
               pinned.value_or(0), group_index);

    _bound_raster_groups[group_index] = vg;

    u32 const buffer_index = 0;
    auto const offset = VkDeviceSize(vg->range.offset);
    _ctx._descriptor_functions.cmd_set_descriptor_offsets(_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                          _bound_raster_layout->_layout, u32(group_index), 1,
                                                          &buffer_index, &offset);
}

void vulkan_command_list::raster_bind_vertex_buffers(int first_slot, cc::span<sg::vertex_buffer_view const> views)
{
    CC_ASSERT(first_slot >= 0, "vertex buffer slot must be non-negative");
    if (views.empty())
        return;

    cc::fixed_vector<VkBuffer, sg::max_vertex_buffers> buffers;
    cc::fixed_vector<VkDeviceSize, sg::max_vertex_buffers> offsets;
    for (int i = 0; i < int(views.size()); ++i)
    {
        auto const& v = views[i];
        CC_ASSERT(v.buffer != nullptr, "vertex_buffer_view has no buffer");
        auto const& buf = as_vulkan_buffer(v.buffer);
        buffers.push_back(buf._buffer);
        offsets.push_back(VkDeviceSize(v.offset_in_bytes));

        // Remember the buffer at its slot so its vertex_read is declared for barriers at draw time.
        // The stride is the pipeline's vertex-input state here, not a bind parameter as it is in D3D12.
        int const slot = first_slot + i;
        while (int(_bound_vertex_buffers.size()) <= slot)
            _bound_vertex_buffers.push_back(nullptr);
        _bound_vertex_buffers[slot] = &buf;
    }

    vkCmdBindVertexBuffers(_buffer, u32(first_slot), u32(buffers.size()), buffers.data(), offsets.data());
}

void vulkan_command_list::raster_bind_index_buffer(sg::index_buffer_view const& view)
{
    CC_ASSERT(view.buffer != nullptr, "index_buffer_view has no buffer");
    auto const& buf = as_vulkan_buffer(view.buffer);
    vkCmdBindIndexBuffer(_buffer, buf._buffer, VkDeviceSize(view.offset_in_bytes),
                         view.format == sg::index_format::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    _bound_index_buffer = &buf;
}

void vulkan_command_list::raster_set_viewport(sg::viewport const& vp)
{
    // Negative height flips the Y axis, which is what makes a shader written for D3D12's top-left origin render the
    // same way here — the one place sg's viewport does not translate field for field.
    auto const v = VkViewport{
        .x = vp.offset[0],
        .y = vp.offset[1] + vp.size[1],
        .width = vp.size[0],
        .height = -vp.size[1],
        .minDepth = vp.min_depth,
        .maxDepth = vp.max_depth,
    };
    vkCmdSetViewport(_buffer, 0, 1, &v);
}

void vulkan_command_list::raster_set_scissor(tg::aabb2i const& rect)
{
    auto const r = VkRect2D{
        .offset = {rect.min[0], rect.min[1]},
        .extent = {u32(rect.max[0] - rect.min[0]), u32(rect.max[1] - rect.min[1])},
    };
    vkCmdSetScissor(_buffer, 0, 1, &r);
}

void vulkan_command_list::raster_set_stencil_reference(u32 reference)
{
    vkCmdSetStencilReference(_buffer, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
}

void vulkan_command_list::raster_set_blend_constants(tg::vec4f constants)
{
    vkCmdSetBlendConstants(_buffer, constants.data);
}

void vulkan_command_list::raster_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset)
{
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before setting inline constants");
    CC_ASSERT(_bound_raster_layout->_inline_constants_bytes > 0, "the bound pipeline layout declares no "
                                                                 "inline_constants block");
    CC_ASSERT(data.size() % 4 == 0, "inline-constants payload size must be a multiple of 4 bytes");

    isize const off = offset.value_or(0);
    CC_ASSERT(off >= 0 && off % 4 == 0, "inline-constants offset must be non-negative and a multiple of 4");
    if (offset.has_value())
        CC_ASSERT(off + data.size() <= isize(_bound_raster_layout->_inline_constants_bytes),
                  "partial inline-constants update exceeds the declared block size");
    else
        CC_ASSERT(data.size() == isize(_bound_raster_layout->_inline_constants_bytes),
                  "full inline-constants replace must match the declared block size");

    vkCmdPushConstants(_buffer, _bound_raster_layout->_layout, VK_SHADER_STAGE_ALL, u32(off), u32(data.size()),
                       data.data());
}

void vulkan_command_list::declare_raster_draw_barriers(bool indexed)
{
    // Bound groups' shader reads/writes, same policy as compute_dispatch, keyed to the graphics stages.
    for (auto const* bound_group : _bound_raster_groups)
    {
        if (bound_group == nullptr)
            continue;

        // The raster scope has no declare_array_*_access yet, so an array binding here would go untracked.
        CC_ASSERT(bound_group->array_bindings.empty(), "array bindings are not supported in raster draws yet");
        for (auto const& view : bound_group->hazard_views)
            if (view.buffer != nullptr)
                track_buffer_access(*view.buffer, sg::pipeline_stage_flag::vertex | sg::pipeline_stage_flag::fragment,
                                    sg::shader_access_of(view.access));
        for (auto const& tv : bound_group->texture_hazard_views)
            (void)track_texture_access(*tv.texture, tv.range,
                                       sg::pipeline_stage_flag::vertex | sg::pipeline_stage_flag::fragment,
                                       sg::shader_access_of(tv.access), sg::shader_layout_of(tv.access));
    }

    // The input assembler reads the bound vertex buffers; an indexed draw also fetches the index buffer.
    for (auto const* vb : _bound_vertex_buffers)
        if (vb != nullptr)
            track_buffer_access(*vb, sg::pipeline_stage_flag::vertex, sg::access_flag::vertex_read);
    if (indexed && _bound_index_buffer != nullptr)
        track_buffer_access(*_bound_index_buffer, sg::pipeline_stage_flag::vertex, sg::access_flag::index_read);
}

void vulkan_command_list::raster_draw(sg::draw_config const& config)
{
    CC_ASSERT(_in_render_pass, "draw requires an open rendering scope");
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before drawing");
    CC_ASSERT(config.vertex_range.offset >= 0 && config.vertex_range.size >= 0, "vertex range must be non-negative");
    CC_ASSERT(config.instance_range.offset >= 0 && config.instance_range.size >= 0, "instance range must be "
                                                                                    "non-negative");

    declare_raster_draw_barriers(false);
    flush_barriers();
    vkCmdDraw(_buffer, u32(config.vertex_range.size), u32(config.instance_range.size), u32(config.vertex_range.offset),
              u32(config.instance_range.offset));
}

void vulkan_command_list::raster_draw_indexed(sg::draw_indexed_config const& config)
{
    CC_ASSERT(_in_render_pass, "draw_indexed requires an open rendering scope");
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before drawing");
    CC_ASSERT(_bound_index_buffer != nullptr, "draw_indexed requires a bound index buffer");
    CC_ASSERT(config.index_range.offset >= 0 && config.index_range.size >= 0, "index range must be non-negative");
    CC_ASSERT(config.instance_range.offset >= 0 && config.instance_range.size >= 0, "instance range must be "
                                                                                    "non-negative");

    declare_raster_draw_barriers(true);
    flush_barriers();
    vkCmdDrawIndexed(_buffer, u32(config.index_range.size), u32(config.instance_range.size),
                     u32(config.index_range.offset), config.vertex_offset, u32(config.instance_range.offset));
}
} // namespace sg::backend::vulkan
