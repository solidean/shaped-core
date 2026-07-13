// dx12_command_list raster rendering scope: transition the color / depth-stencil targets to their
// render-target / depth-stencil layouts, bind them to the output-merger, and apply each target's clear /
// discard. There is
// no graphics pipeline yet, so a scope only applies its begin-ops; draw recording lands with the pipeline.

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/fixed_vector.hh>
#include <shaped-graphics/backend/access_inference.hh> // shader_access_of / shader_layout_of
#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_epoch.hh>
#include <shaped-graphics/backends/dx12/dx12_pipeline_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_raster_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/command_list.raster.hh>
#include <shaped-graphics/pixel_format.hh>

#include <memory> // std::dynamic_pointer_cast

namespace sg::backend::dx12
{
namespace
{
[[nodiscard]] dx12_texture_handle as_dx12_texture(sg::raw_texture_handle const& tex)
{
    // Only dx12 textures ever reach a dx12 command list, so static_cast is sound; the dynamic_cast is the
    // debug-only check (stripped in release, where CC_ASSERT leaves its condition unevaluated).
    CC_ASSERT(std::dynamic_pointer_cast<dx12_texture const>(tex) != nullptr, "target texture is not a dx12 "
                                                                             "texture");
    return std::static_pointer_cast<dx12_texture const>(tex);
}

[[nodiscard]] dx12_buffer_handle as_dx12_buffer(sg::raw_buffer_handle const& buf)
{
    CC_ASSERT(std::dynamic_pointer_cast<dx12_buffer const>(buf) != nullptr, "buffer is not a dx12 buffer");
    return std::static_pointer_cast<dx12_buffer const>(buf);
}
} // namespace

void dx12_command_list::raster_begin_rendering(sg::rendering_info const& info)
{
    CC_ASSERT(!_in_render_pass, "begin_rendering called while a rendering scope is already open");
    CC_ASSERT(!info.color_targets.empty() || info.depth_stencil_target.has_value(),
              "a rendering scope needs at least one color or depth-stencil target");

    // 1) Transition each target to its output layout (color -> render_target, depth -> depth_readwrite)
    //    and register it with this list. Flush the barriers up-front: enhanced barriers are illegal once the
    //    targets are bound / inside the pass, unlike a compute dispatch which flushes right before each op.
    for (auto const& ct : info.color_targets)
        track_texture_access(as_dx12_texture(ct.view.texture()), ct.view.range(), sg::pipeline_stage_flags::render_target,
                             sg::access_flags::color_write, sg::texture_layout::render_target);
    if (info.depth_stencil_target.has_value())
    {
        // The depth_readwrite layout (LAYOUT_DEPTH_STENCIL_WRITE) permits only DEPTH_STENCIL_WRITE access —
        // pairing it with depth_read is rejected. A read-only depth target (once draws exist) would use the
        // depth_readonly layout + depth_read instead.
        auto const& dt = info.depth_stencil_target.value();
        track_texture_access(as_dx12_texture(dt.view.texture()), dt.view.range(),
                             sg::pipeline_stage_flags::depth_stencil_target, sg::access_flags::depth_write,
                             sg::texture_layout::depth_readwrite);
    }
    flush_barriers();

    // 2) Create the RTV/DSV descriptors and collect the CPU handles the clears / OM bind use. The slots
    //    outlive recording and are freed against this list's epoch in end_rendering.
    cc::fixed_vector<D3D12_CPU_DESCRIPTOR_HANDLE, sg::max_color_targets> rtv_handles;
    for (auto const& ct : info.color_targets)
    {
        auto rtv = _ctx.create_dx12_render_target_view(ct.view);
        CC_ASSERT(rtv.has_value(), "failed to create a render-target view (RTV heap exhausted?)");
        rtv_handles.push_back(rtv.value().handle);
        _rendering_rtv_slots.push_back(rtv.value().slot);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = {};
    if (info.depth_stencil_target.has_value())
    {
        auto dsv = _ctx.create_dx12_depth_stencil_view(info.depth_stencil_target.value().view);
        CC_ASSERT(dsv.has_value(), "failed to create a depth-stencil view (DSV heap exhausted?)");
        dsv_handle = dsv.value().handle;
        _rendering_dsv_slot = dsv.value().slot;
    }

    // 3) Apply each target's begin-op: clear to a value, discard (contents undefined), or preserve (nothing).
    for (int i = 0; i < int(info.color_targets.size()); ++i)
    {
        auto const& ct = info.color_targets[i];
        if (ct.op == sg::target_op::clear)
            _list->ClearRenderTargetView(rtv_handles[i], ct.clear_color.data, 0, nullptr);
        else if (ct.op == sg::target_op::discard)
            _list->DiscardResource(as_dx12_texture(ct.view.texture())->_resource.Get(), nullptr);
    }
    if (info.depth_stencil_target.has_value())
    {
        auto const& dt = info.depth_stencil_target.value();
        if (dt.op == sg::target_op::clear)
        {
            UINT clear_flags = D3D12_CLEAR_FLAG_DEPTH;
            if (sg::has_stencil(dt.view.format()))
                clear_flags |= D3D12_CLEAR_FLAG_STENCIL;
            _list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAGS(clear_flags), dt.clear_depth, dt.clear_stencil,
                                         0, nullptr);
        }
        else if (dt.op == sg::target_op::discard)
            _list->DiscardResource(as_dx12_texture(dt.view.texture())->_resource.Get(), nullptr);
    }

    // 4) Bind the targets to the output-merger.
    _list->OMSetRenderTargets(UINT(rtv_handles.size()), rtv_handles.empty() ? nullptr : rtv_handles.data(), FALSE,
                              info.depth_stencil_target.has_value() ? &dsv_handle : nullptr);

    // 5) Viewport + scissor — default to the full target extent (the common "render to the whole target" case).
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

    D3D12_VIEWPORT vp = {};
    if (info.viewport.has_value())
    {
        auto const& v = info.viewport.value();
        vp = {v.offset[0], v.offset[1], v.size[0], v.size[1], v.min_depth, v.max_depth};
    }
    else
        vp = {0.0f, 0.0f, float(extent_w), float(extent_h), 0.0f, 1.0f};
    _list->RSSetViewports(1, &vp);

    D3D12_RECT rect = {};
    if (info.scissor.has_value())
    {
        auto const& s = info.scissor.value();
        rect = {LONG(s.min[0]), LONG(s.min[1]), LONG(s.max[0]), LONG(s.max[1])};
    }
    else
        rect = {0, 0, LONG(extent_w), LONG(extent_h)};
    _list->RSSetScissorRects(1, &rect);

    _in_render_pass = true;
}

void dx12_command_list::raster_end_rendering()
{
    CC_ASSERT(_in_render_pass, "end_rendering called with no open rendering scope");

    // Unbind the targets from the output-merger: a later copy / dispatch in this list transitions them to
    // another layout, which D3D12 forbids while they are still bound as render / depth targets.
    _list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    // The RTV/DSV descriptor slots must outlive the list's GPU execution (the recorded commands read them
    // when they run). Free them against the recording epoch — this list's submit epoch — so the finalizer
    // runs only once that epoch retires, i.e. after the GPU finishes. RTV/DSV slots own no ID3D12Resource,
    // so the expiring resource carries only finalizers.
    if (!_rendering_rtv_slots.empty() || _rendering_dsv_slot != cpu_descriptor_slot::invalid)
    {
        dx12_expiring_resource expiring;
        for (auto const slot : _rendering_rtv_slots)
            expiring.finalizers.push_back([ctx = &_ctx, slot]() { ctx->free_dx12_render_target_view(slot); });
        if (_rendering_dsv_slot != cpu_descriptor_slot::invalid)
            expiring.finalizers.push_back([ctx = &_ctx, slot = _rendering_dsv_slot]()
                                          { ctx->free_dx12_depth_stencil_view(slot); });
        _ctx.schedule_deferred_deletion(cc::move(expiring));
    }

    _rendering_rtv_slots.clear();
    _rendering_dsv_slot = cpu_descriptor_slot::invalid;
    _in_render_pass = false;

    // The graphics bind + IA state is scoped to the pass it was set up in.
    _bound_raster_layout = nullptr;
    _bound_raster_groups.clear();
    _bound_vertex_buffers.clear();
    _bound_index_buffer = nullptr;
}

// --- draw recording (reached through cmd.raster / cmd.raster.manual) -----------------------------------

void dx12_command_list::raster_bind_pipeline(sg::raster_pipeline const& pipeline)
{
    CC_ASSERT(_in_render_pass, "bind_pipeline requires an open rendering scope");
    auto const* rp = dynamic_cast<dx12_raster_pipeline const*>(&pipeline);
    CC_ASSERT(rp != nullptr, "raster_pipeline is not a dx12 raster_pipeline");

    // The shader-visible heaps must be set before any root descriptor table is bound (as for compute).
    ID3D12DescriptorHeap* heaps[] = {_ctx._descriptor_heap.heap.Get(), _ctx._sampler_heap.heap.Get()};
    _list->SetDescriptorHeaps(2, heaps);
    _list->SetGraphicsRootSignature(rp->layout->root_signature.Get());
    _list->SetPipelineState(rp->pipeline_state.Get());
    _list->IASetPrimitiveTopology(rp->topology);

    _bound_raster_layout = rp->layout.get();
    _bound_raster_groups.clear_resize_to_filled(_bound_raster_layout->groups.size(), nullptr);
}

void dx12_command_list::raster_bind_group(int set, sg::binding_group const& group)
{
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before binding groups");
    CC_ASSERT(set >= 0 && set < int(_bound_raster_groups.size()), "binding-group slot out of range for the bound "
                                                                  "pipeline layout");

    auto const* dg = dynamic_cast<dx12_binding_group const*>(&group);
    CC_ASSERT(dg != nullptr, "binding_group is not a dx12 binding_group");
    CC_ASSERT(!(dg->transient && dg->creation_epoch != _ctx.current_epoch()),
              "transient binding_group used past its epoch (its descriptors have been recycled)");

    auto const& gslot = _bound_raster_layout->groups[set];
    CC_ASSERT(dg->layout == gslot.layout, "binding_group's layout does not match the pipeline layout's slot");

    // Remembered so its views' accesses are declared at draw (like compute). Root-parameter indices come
    // from the pipeline layout's slot; graphics uses the graphics root bind point.
    _bound_raster_groups[set] = dg;
    if (gslot.resource_root_param >= 0)
        _list->SetGraphicsRootDescriptorTable(UINT(gslot.resource_root_param), dg->table_start);
    if (gslot.sampler_root_param >= 0)
        _list->SetGraphicsRootDescriptorTable(UINT(gslot.sampler_root_param), dg->sampler_table_start);
}

void dx12_command_list::raster_bind_vertex_buffers(int first_slot, cc::span<sg::vertex_buffer_view const> views)
{
    CC_ASSERT(first_slot >= 0, "vertex buffer slot must be non-negative");

    cc::fixed_vector<D3D12_VERTEX_BUFFER_VIEW, sg::max_vertex_buffers> vbvs;
    for (int i = 0; i < int(views.size()); ++i)
    {
        auto const& v = views[i];
        CC_ASSERT(v.buffer != nullptr, "vertex_buffer_view has no buffer");
        auto buf = as_dx12_buffer(v.buffer);
        cc::isize const size = v.size_in_bytes < 0 ? (buf->size_in_bytes() - v.offset_in_bytes) : v.size_in_bytes;

        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.BufferLocation = buf->gpu_virtual_address() + UINT64(v.offset_in_bytes);
        vbv.SizeInBytes = UINT(size);
        vbv.StrideInBytes = UINT(v.stride_in_bytes);
        vbvs.push_back(vbv);

        // Remember the buffer at its slot so its vertex_read is declared for barriers at draw time.
        int const slot = first_slot + i;
        while (int(_bound_vertex_buffers.size()) <= slot)
            _bound_vertex_buffers.push_back(nullptr);
        _bound_vertex_buffers[slot] = cc::move(buf);
    }

    _list->IASetVertexBuffers(UINT(first_slot), UINT(vbvs.size()), vbvs.empty() ? nullptr : vbvs.data());
}

void dx12_command_list::raster_bind_index_buffer(sg::index_buffer_view const& view)
{
    CC_ASSERT(view.buffer != nullptr, "index_buffer_view has no buffer");
    auto buf = as_dx12_buffer(view.buffer);
    cc::isize const size = view.size_in_bytes < 0 ? (buf->size_in_bytes() - view.offset_in_bytes) : view.size_in_bytes;

    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = buf->gpu_virtual_address() + UINT64(view.offset_in_bytes);
    ibv.SizeInBytes = UINT(size);
    ibv.Format = view.format == sg::index_format::uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    _list->IASetIndexBuffer(&ibv);

    _bound_index_buffer = cc::move(buf);
}

void dx12_command_list::raster_set_viewport(sg::viewport const& vp)
{
    D3D12_VIEWPORT d = {vp.offset[0], vp.offset[1], vp.size[0], vp.size[1], vp.min_depth, vp.max_depth};
    _list->RSSetViewports(1, &d);
}

void dx12_command_list::raster_set_scissor(tg::aabb2i const& rect)
{
    D3D12_RECT r = {LONG(rect.min[0]), LONG(rect.min[1]), LONG(rect.max[0]), LONG(rect.max[1])};
    _list->RSSetScissorRects(1, &r);
}

void dx12_command_list::raster_set_stencil_reference(sg::u32 reference)
{
    _list->OMSetStencilRef(UINT(reference));
}

void dx12_command_list::raster_set_blend_constants(tg::vec4f constants)
{
    _list->OMSetBlendFactor(constants.data);
}

void dx12_command_list::raster_set_inline_constants(cc::span<cc::byte const> data, cc::optional<cc::isize> offset)
{
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before setting inline constants");
    CC_ASSERT(_bound_raster_layout->inline_constants_root_param >= 0, "the bound pipeline layout declares no "
                                                                      "inline_constants block");
    CC_ASSERT(data.size() % 4 == 0, "inline-constants payload size must be a multiple of 4 bytes");

    cc::isize const off = offset.value_or(0);
    CC_ASSERT(off >= 0 && off % 4 == 0, "inline-constants offset must be non-negative and a multiple of 4");
    if (offset.has_value())
        CC_ASSERT(off + data.size() <= cc::isize(_bound_raster_layout->inline_constants_num_32bit) * 4,
                  "partial inline-constants update exceeds the declared block size");
    else
        CC_ASSERT(data.size() == cc::isize(_bound_raster_layout->inline_constants_num_32bit) * 4,
                  "full inline-constants replace must match the declared block size");

    _list->SetGraphicsRoot32BitConstants(UINT(_bound_raster_layout->inline_constants_root_param), UINT(data.size() / 4),
                                         data.data(), UINT(off / 4));
}

void dx12_command_list::declare_raster_draw_barriers(bool indexed)
{
    // Bound groups' shader reads/writes (same policy as compute_dispatch), keyed to the graphics stages.
    for (auto const* bound_group : _bound_raster_groups)
    {
        if (bound_group == nullptr)
            continue;
        for (auto const& view : bound_group->hazard_views)
            if (view.buffer)
                track_buffer_access(view.buffer, sg::pipeline_stage_flags::vertex | sg::pipeline_stage_flags::fragment,
                                    sg::shader_access_of(view.access));
        for (auto const& tv : bound_group->texture_hazard_views)
            track_texture_access(tv.texture, tv.range,
                                 sg::pipeline_stage_flags::vertex | sg::pipeline_stage_flags::fragment,
                                 sg::shader_access_of(tv.access), sg::shader_layout_of(tv.access));
    }

    // The IA vertex fetch reads the bound vertex buffers; an indexed draw also fetches the index buffer.
    for (auto const& vb : _bound_vertex_buffers)
        if (vb)
            track_buffer_access(vb, sg::pipeline_stage_flags::vertex, sg::access_flags::vertex_read);
    if (indexed && _bound_index_buffer)
        track_buffer_access(_bound_index_buffer, sg::pipeline_stage_flags::vertex, sg::access_flags::index_read);
}

void dx12_command_list::raster_draw(sg::draw_config const& config)
{
    CC_ASSERT(_in_render_pass, "draw requires an open rendering scope");
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before drawing");
    CC_ASSERT(config.vertex_range.offset >= 0 && config.vertex_range.size >= 0, "vertex range must be non-negative");
    CC_ASSERT(config.instance_range.offset >= 0 && config.instance_range.size >= 0, "instance range must be "
                                                                                    "non-negative");

    declare_raster_draw_barriers(false);
    flush_barriers();
    _list->DrawInstanced(UINT(config.vertex_range.size), UINT(config.instance_range.size),
                         UINT(config.vertex_range.offset), UINT(config.instance_range.offset));
}

void dx12_command_list::raster_draw_indexed(sg::draw_indexed_config const& config)
{
    CC_ASSERT(_in_render_pass, "draw_indexed requires an open rendering scope");
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before drawing");
    CC_ASSERT(_bound_index_buffer != nullptr, "draw_indexed requires a bound index buffer");
    CC_ASSERT(config.index_range.offset >= 0 && config.index_range.size >= 0, "index range must be non-negative");
    CC_ASSERT(config.instance_range.offset >= 0 && config.instance_range.size >= 0, "instance range must be "
                                                                                    "non-negative");

    declare_raster_draw_barriers(true);
    flush_barriers();
    _list->DrawIndexedInstanced(UINT(config.index_range.size), UINT(config.instance_range.size),
                                UINT(config.index_range.offset), config.vertex_offset,
                                UINT(config.instance_range.offset));
}
} // namespace sg::backend::dx12
