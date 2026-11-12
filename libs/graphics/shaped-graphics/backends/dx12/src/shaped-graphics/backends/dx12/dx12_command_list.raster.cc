// dx12_command_list raster recording.
// The rendering scope transitions the color / depth-stencil targets to their render-target / depth-stencil layouts, binds them to the output-merger, and applies each target's clear / discard.
// Also the raster bind path: pipeline and groups, IA buffers, dynamic state, and draw recording.

#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/fixed_vector.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_compute_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_epoch.hh>
#include <shaped-graphics/backends/dx12/dx12_pipeline_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_raster_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/backends/dx12/dx12_texture_access.hh> // subresource_extent_of
#include <shaped-graphics/backends/dx12/dx12_view_desc.hh>      // create_render_target_view / create_depth_stencil_view
#include <shaped-graphics/barrier/access_inference.hh>          // shader_access_of / shader_layout_of
#include <shaped-graphics/command_list/raster.hh>
#include <shaped-graphics/resource/pixel_format.hh>


namespace sg::backend::dx12
{
namespace
{
[[nodiscard]] dx12_texture_handle as_dx12_texture(sg::raw_texture_handle const& tex)
{
    // Only dx12 textures ever reach a dx12 command list, so static_cast is sound.
    // The dynamic_cast is the debug-only check, stripped in release where CC_ASSERT leaves its condition unevaluated.
    CC_ASSERT(std::dynamic_pointer_cast<dx12_texture const>(tex) != nullptr, "target texture is not a dx12 "
                                                                             "texture");
    return std::static_pointer_cast<dx12_texture const>(tex);
}

[[nodiscard]] dx12_buffer_handle as_dx12_buffer(sg::raw_buffer_handle const& buf)
{
    CC_ASSERT(std::dynamic_pointer_cast<dx12_buffer const>(buf) != nullptr, "buffer is not a dx12 buffer");
    return std::static_pointer_cast<dx12_buffer const>(buf);
}

/// Discards exactly the subresources a target's view names.
///
/// A null discard region means EVERY subresource of the resource, and a rendering scope transitions only the ones
/// its view covers — so a view of one mip level discards the levels beside it while they are still in whatever
/// layout their last use left them in, which the runtime rejects rather than ignores.
/// Only the mip axis is contiguous in D3D12's subresource numbering, so an arbitrary range takes one region per
/// (aspect plane, array slice).
void discard_target_subresources(ID3D12GraphicsCommandList* list,
                                 dx12_texture_handle const& texture,
                                 sg::subresource_range const& range)
{
    auto const extent = subresource_extent_of(texture->description());
    for (auto aspect = range.aspect_range.start; aspect < range.aspect_range.end; ++aspect)
        for (auto layer = range.array_range.start; layer < range.array_range.end; ++layer)
        {
            auto const first
                = range.mip_range.start + layer * extent.mip_count + aspect * extent.mip_count * extent.array_count;
            D3D12_DISCARD_REGION const region = {.NumRects = 0,
                                                 .pRects = nullptr,
                                                 .FirstSubresource = UINT(first),
                                                 .NumSubresources = UINT(range.mip_range.end - range.mip_range.start)};
            list->DiscardResource(texture->_resource.Get(), &region);
        }
}
} // namespace

void dx12_command_list::raster_begin_rendering(sg::rendering_info const& info)
{
    CC_ASSERT(!_in_render_pass, "begin_rendering called while a rendering scope is already open");
    CC_ASSERT(!info.color_targets.empty() || info.depth_stencil_target.has_value(),
              "a rendering scope needs at least one color or depth-stencil target");

    // 1) Transition each target to its output layout (color -> render_target, depth -> depth_readwrite) and register it with this list.
    //    Flush the barriers up-front: enhanced barriers are illegal once the targets are bound, unlike a compute dispatch which flushes right before each op.
    for (auto const& ct : info.color_targets)
        track_texture_access(as_dx12_texture(ct.view.texture()), ct.view.range(), sg::pipeline_stage_flag::render_target,
                             sg::access_flag::color_write, sg::texture_layout::render_target);
    if (info.depth_stencil_target.has_value())
    {
        // The depth_readwrite layout (LAYOUT_DEPTH_STENCIL_WRITE) permits only DEPTH_STENCIL_WRITE access, so pairing it with depth_read is rejected.
        // depth_read pairs with the depth_readonly layout; this scope binds every depth target read-write.
        auto const& dt = info.depth_stencil_target.value();
        track_texture_access(as_dx12_texture(dt.view.texture()), dt.view.range(),
                             sg::pipeline_stage_flag::depth_stencil_target, sg::access_flag::depth_write,
                             sg::texture_layout::depth_readwrite);
    }
    flush_barriers();

    // 2) Write the RTV/DSV descriptors into this list's own slots and collect the CPU handles the clears / OM bind use.
    //    Overwriting what the previous scope wrote there is safe, since D3D12 read those when it recorded them.
    cc::fixed_vector<D3D12_CPU_DESCRIPTOR_HANDLE, sg::max_color_targets> rtv_handles;
    for (auto i = 0; i < int(info.color_targets.size()); ++i)
    {
        if (i == int(_rtv_slots.size()))
        {
            auto const slot = _ctx._rtv_heap->allocate();
            CC_ASSERT(slot != cpu_descriptor_slot::invalid, "RTV descriptor heap exhausted");
            _rtv_slots.push_back(slot);
        }
        auto const handle = _ctx._rtv_heap->cpu_at(_rtv_slots[i]);
        create_render_target_view(_ctx._device.Get(), info.color_targets[i].view, handle);
        rtv_handles.push_back(handle);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = {};
    if (info.depth_stencil_target.has_value())
    {
        if (_dsv_slot == cpu_descriptor_slot::invalid)
        {
            _dsv_slot = _ctx._dsv_heap->allocate();
            CC_ASSERT(_dsv_slot != cpu_descriptor_slot::invalid, "DSV descriptor heap exhausted");
        }
        dsv_handle = _ctx._dsv_heap->cpu_at(_dsv_slot);
        create_depth_stencil_view(_ctx._device.Get(), info.depth_stencil_target.value().view, dsv_handle);
    }

    // 3) Apply each target's begin-op: clear to a value, discard (contents undefined), or preserve (nothing).
    for (int i = 0; i < int(info.color_targets.size()); ++i)
    {
        auto const& ct = info.color_targets[i];
        if (ct.op == sg::target_op::clear)
            _list->ClearRenderTargetView(rtv_handles[i], ct.clear_color.data, 0, nullptr);
        else if (ct.op == sg::target_op::discard)
            discard_target_subresources(_list.Get(), as_dx12_texture(ct.view.texture()), ct.view.range());
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
            discard_target_subresources(_list.Get(), as_dx12_texture(dt.view.texture()), dt.view.range());
    }

    // 4) Bind the targets to the output-merger.
    _list->OMSetRenderTargets(UINT(rtv_handles.size()), rtv_handles.empty() ? nullptr : rtv_handles.data(), FALSE,
                              info.depth_stencil_target.has_value() ? &dsv_handle : nullptr);

    // No pipeline asks for view instance masking, so none reads this; it states the views they all render anyway.
    if (info.view_count > 1)
    {
        ComPtr<ID3D12GraphicsCommandList1> list1;
        CC_ASSERT(SUCCEEDED(_list.As(&list1)), "view instancing needs ID3D12GraphicsCommandList1");
        list1->SetViewInstanceMask((1u << UINT(info.view_count)) - 1u);
    }

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

    // The blend constants are 0 until set, per rendering scope, so one scope's never leaks into the next.
    raster_set_blend_constants(tg::vec4f(0, 0, 0, 0));

    _in_render_pass = true;
}

void dx12_command_list::raster_end_rendering()
{
    CC_ASSERT(_in_render_pass, "end_rendering called with no open rendering scope");

    // Unbind the targets from the output-merger: a later copy / dispatch in this list transitions them to another layout, which D3D12 forbids while they are bound as render / depth targets.
    _list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    _in_render_pass = false;

    // The graphics bind + IA state is scoped to the pass it was set up in.
    _bound_raster_layout = nullptr;
    _bound_raster_groups.clear();
    _bound_raster_footprint = nullptr;
    _bound_raster_footprint_owner = nullptr;
    _bound_vertex_buffers.clear();
    _bound_index_buffer = nullptr;

    // The core ended the values' half already; this is the list's.
    _raster_arrays.end_all();
}

// --- draw recording (reached through cmd.raster / cmd.raster.manual) -----------------------------------

void dx12_command_list::raster_bind_pipeline(sg::raster_pipeline const& pipeline)
{
    keep_bound(pipeline.weak_from_this().lock());
    CC_ASSERT(_in_render_pass, "bind_pipeline requires an open rendering scope");
    auto const* rp = dynamic_cast<dx12_raster_pipeline const*>(&pipeline);
    CC_ASSERT(rp != nullptr, "raster_pipeline is not a dx12 raster_pipeline");

    // The shader-visible heaps must be set before any root descriptor table is bound (as for compute).
    ID3D12DescriptorHeap* heaps[] = {_ctx._descriptor_heap.heap.Get(), _ctx._sampler_heap.heap.Get()};
    _list->SetDescriptorHeaps(2, heaps);
    _list->SetGraphicsRootSignature(rp->layout->root_signature.Get());
    _list->SetPipelineState(rp->pipeline_state.Get());
    _bound_raster_pso = rp->pipeline_state.Get();
    bind_debug_window(*rp->layout, pipeline.footprint(), true);
    _list->IASetPrimitiveTopology(rp->topology);
    _raster_inline_constants.clear_resize_to_filled(rp->layout->inline_constants_num_32bit, 0u);

    _bound_raster_layout = rp->layout.get();
    _bound_raster_groups.clear_resize_to_filled(_bound_raster_layout->groups.size(), nullptr);
    _bound_raster_footprint = &pipeline.footprint();
    _bound_raster_footprint_owner = &pipeline;
}

void dx12_command_list::raster_bind_group(int group_index, sg::binding_group const& group)
{
    keep_bound(group.weak_from_this().lock());
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before binding groups");
    CC_ASSERT(group_index >= 0 && group_index < int(_bound_raster_groups.size()),
              "binding-group slot out of range for the bound "
              "pipeline layout");

    auto const* dg = dynamic_cast<dx12_binding_group const*>(&group);
    CC_ASSERT(dg != nullptr, "binding_group is not a dx12 binding_group");
    CC_ASSERT(!(dg->transient && dg->creation_epoch != _ctx.current_epoch()),
              "transient binding_group used past its epoch (its descriptors have been recycled)");

    auto const& gslot = _bound_raster_layout->groups[group_index];
    CC_ASSERTF(dg->layout == gslot.layout, "{}",
               sg::impl::describe_layout_mismatch(group_index, gslot.layout.get(), dg->layout.get()));
    // A layout whose bindings name a descriptor set may only be bound at that one slot.
    auto const pinned = dg->layout->group_index();
    CC_ASSERTF(!pinned.has_value() || pinned.value() == u32(group_index),
               "binding_group is pinned to group index {} by its bindings and cannot be bound at slot {}",
               pinned.value_or(0), group_index);

    // Remembered so its views' accesses are declared at draw (like compute). Root-parameter indices come
    // from the pipeline layout's slot; graphics uses the graphics root bind point.
    _bound_raster_groups[group_index] = dg;
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
        isize const size = v.size_in_bytes < 0 ? (buf->size_in_bytes() - v.offset_in_bytes) : v.size_in_bytes;

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
    CC_ASSERT(view.offset_in_bytes % sg::index_buffer_offset_alignment == 0,
              "an index_buffer_view's offset must be 4-byte aligned — see sg::index_buffer_offset_alignment");
    auto buf = as_dx12_buffer(view.buffer);
    isize const size = view.size_in_bytes < 0 ? (buf->size_in_bytes() - view.offset_in_bytes) : view.size_in_bytes;

    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = buf->gpu_virtual_address() + UINT64(view.offset_in_bytes);
    ibv.SizeInBytes = UINT(size);
    ibv.Format = view.format == sg::index_format::uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    _list->IASetIndexBuffer(&ibv);

    _bound_index_buffer = cc::move(buf);
    _index_format = view.format;
    _index_view_offset_in_bytes = view.offset_in_bytes;
}

u32 dx12_command_list::raster_declare_array_buffer_access(cc::string_view binding_name,
                                                          cc::span<sg::array_buffer_access const> elements)
{
    CC_ASSERT(_in_render_pass, "declare_array_buffer_access requires an open rendering scope");
    CC_ASSERT(!binding_name.empty(), "declare_array_buffer_access requires a binding name");
    auto const id = next_array_declaration_id();
    _raster_arrays.declare_buffers(id, binding_name, elements);
    return id;
}

u32 dx12_command_list::raster_declare_array_texture_access(cc::string_view binding_name,
                                                           cc::span<sg::array_texture_access const> elements)
{
    CC_ASSERT(_in_render_pass, "declare_array_texture_access requires an open rendering scope");
    CC_ASSERT(!binding_name.empty(), "declare_array_texture_access requires a binding name");
    auto const id = next_array_declaration_id();
    _raster_arrays.declare_textures(id, binding_name, elements);
    return id;
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

void dx12_command_list::raster_set_stencil_reference(u32 reference)
{
    _list->OMSetStencilRef(UINT(reference));
}

void dx12_command_list::raster_set_blend_constants(tg::vec4f constants)
{
    _list->OMSetBlendFactor(constants.data);
}

void dx12_command_list::raster_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset)
{
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before setting inline constants");
    CC_ASSERT(_bound_raster_layout->inline_constants_root_param >= 0, "the bound pipeline layout declares no "
                                                                      "inline_constants block");
    CC_ASSERT(data.size() % 4 == 0, "inline-constants payload size must be a multiple of 4 bytes");

    isize const off = offset.value_or(0);
    CC_ASSERT(off >= 0 && off % 4 == 0, "inline-constants offset must be non-negative and a multiple of 4");
    if (offset.has_value())
        CC_ASSERT(off + data.size() <= isize(_bound_raster_layout->inline_constants_num_32bit) * 4,
                  "partial inline-constants update exceeds the declared block size");
    else
        CC_ASSERT(data.size() == isize(_bound_raster_layout->inline_constants_num_32bit) * 4,
                  "full inline-constants replace must match the declared block size");

    _list->SetGraphicsRoot32BitConstants(UINT(_bound_raster_layout->inline_constants_root_param), UINT(data.size() / 4),
                                         data.data(), UINT(off / 4));
    if (_raster_inline_constants.size() * 4 >= off + data.size())
        cc::memcpy(_raster_inline_constants.data() + off / 4, data.data(), size_t(data.size()));
}

void dx12_command_list::declare_raster_draw_barriers(bool indexed)
{
    // Bound groups' shader reads/writes (same policy as compute_dispatch), keyed to the graphics stages.
    // Array bindings are tracked as the caller declared them, at the stages the code touches each in.
    auto const graphics_stages = sg::pipeline_stage_flag::vertex | sg::pipeline_stage_flag::fragment;
    bind_software_trace(*_bound_raster_layout, _bound_raster_groups, true);
    declare_group_accesses(_bound_raster_groups, _bound_raster_footprint, graphics_stages);
    declare_array_accesses(_bound_raster_groups, _bound_raster_footprint, _bound_raster_footprint_owner,
                           graphics_stages, _raster_arrays);

    // The IA vertex fetch reads the bound vertex buffers; an indexed draw also fetches the index buffer.
    for (auto const& vb : _bound_vertex_buffers)
        if (vb)
            track_buffer_access(vb, sg::pipeline_stage_flag::vertex, sg::access_flag::vertex_read);
    if (indexed && _bound_index_buffer)
        track_buffer_access(_bound_index_buffer, sg::pipeline_stage_flag::vertex, sg::access_flag::index_read);
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

void dx12_command_list::raster_draw_mesh_tasks(int x, int y, int z)
{
    CC_ASSERT(_in_render_pass, "draw_mesh_tasks requires an open rendering scope");
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before drawing");
    declare_raster_draw_barriers(false);
    flush_barriers();
    ComPtr<ID3D12GraphicsCommandList6> list6;
    auto const queried = _list->QueryInterface(IID_PPV_ARGS(&list6));
    CC_ASSERT(SUCCEEDED(queried), "a device with mesh shaders records them through ID3D12GraphicsCommandList6");
    list6->DispatchMesh(UINT(x), UINT(y), UINT(z));
}

void dx12_command_list::raster_draw_mesh_polyfilled(sg::impl::mesh_polyfill_state const& polyfill, int x, int y, int z)
{
    draw_mesh_polyfilled(polyfill, {x, y, z}, nullptr, 0);
}

void dx12_command_list::raster_draw_mesh_polyfilled_indirect(sg::impl::mesh_polyfill_state const& polyfill,
                                                             sg::raw_buffer_handle const& args,
                                                             isize offset_in_bytes,
                                                             isize count)
{
    auto const buffer = std::dynamic_pointer_cast<dx12_buffer const>(args);
    CC_ASSERT(buffer != nullptr, "indirect mesh draw arguments are not a dx12 buffer");
    CC_ASSERT(!buffer->is_expired(), "indirect mesh draw arguments are a transient buffer used past its epoch");
    keep_bound(buffer);
    for (auto i = isize(0); i < count; ++i)
        draw_mesh_polyfilled(polyfill, {0, 0, 0}, buffer, offset_in_bytes + i * isize(sizeof(sg::dispatch_args)));
}

void dx12_command_list::draw_mesh_polyfilled(sg::impl::mesh_polyfill_state const& polyfill,
                                             cc::fixed_array<int, 3> grid,
                                             dx12_buffer_handle const& args,
                                             isize offset_in_bytes)
{
    CC_ASSERT(_in_render_pass, "draw_mesh_tasks requires an open rendering scope");
    CC_ASSERT(_bound_raster_layout != nullptr && _bound_raster_pso != nullptr, "bind a raster pipeline before drawing");
    keep_bound(polyfill.compute);
    keep_bound(polyfill.task);
    keep_bound(polyfill.launched);
    keep_bound(polyfill.scan);

    // Buffers of the draw's own, so no draw's compute forms wait on an earlier one's reads; records launch the mesh
    // workgroups behind a task stage and on an indirect draw, whose counts the GPU then writes into them.
    auto const by_records = polyfill.task != nullptr || args != nullptr;
    auto const groups = i64(grid[0]) * i64(grid[1]) * i64(grid[2]);
    auto const slots = !by_records     ? groups
                     : args != nullptr ? i64(polyfill.indirect_capacity())
                                       : i64(polyfill.task_capacity(isize(groups)));
    auto const held = polyfill.task == nullptr ? i64(1) : args != nullptr ? i64(polyfill.indirect_capacity()) : groups;
    auto const scratch = std::dynamic_pointer_cast<dx12_buffer const>(_ctx.transient.create_raw_buffer(
        isize(slots * polyfill.words_per_group * 4), sg::buffer_usage::readwrite_buffer));
    CC_ASSERT(scratch != nullptr, "the mesh polyfill's slot buffer is not a dx12 buffer");
    keep_bound(scratch);
    auto records = std::shared_ptr<dx12_buffer const>();
    if (by_records)
    {
        records = std::dynamic_pointer_cast<dx12_buffer const>(_ctx.transient.create_raw_buffer(
            isize((12 + held * polyfill.record_words) * 4),
            sg::buffer_usage::readwrite_buffer | sg::buffer_usage::indirect_command_buffer | sg::buffer_usage::copy_dst));
        CC_ASSERT(records != nullptr, "the mesh polyfill's record buffer is not a dx12 buffer");
        keep_bound(records);

        // the header's task grid, and an indirect draw's one record with no task stage, before any form reads them
        auto const upload = [&](cc::span<u32 const> words, isize word)
        { upload_bytes_to_buffer(records, cc::as_bytes(words), word * 4); };
        auto const copy_args = [&](isize word)
        {
            track_buffer_access(args, sg::pipeline_stage_flag::copy, sg::access_flag::copy_read);
            track_buffer_access(records, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
            flush_barriers();
            _list->CopyBufferRegion(records->_resource.Get(), UINT64(word * 4), args->_resource.Get(),
                                    UINT64(offset_in_bytes), UINT64(sizeof(sg::dispatch_args)));
        };
        if (args == nullptr)
        {
            u32 const task_grid[] = {u32(grid[0]), u32(grid[1]), u32(grid[2])};
            upload(task_grid, 9);
        }
        else if (polyfill.task != nullptr)
            copy_args(9);
        else
        {
            u32 const one[] = {1, 1, 1};
            upload(one, 9);
            copy_args(12);
        }
    }

    // Each compute form over the groups bound for the draw, with its inline constants, the grid where it reads one,
    // and the slots and records its layout holds.
    // D3D12 dispatches inside the scope, which binds its targets with OMSetRenderTargets rather than a render pass.
    auto const bind_half = [&](sg::compute_pipeline const& pipeline, cc::optional<cc::fixed_array<u32, 4>> grid_threads)
    {
        auto const* const half = dynamic_cast<dx12_compute_pipeline const*>(&pipeline);
        CC_ASSERT(half != nullptr, "a polyfill's compute form is not a dx12 compute_pipeline");
        auto const& layout = *half->layout;
        _list->SetComputeRootSignature(layout.root_signature.Get());
        _list->SetPipelineState(half->pipeline_state.Get());
        bind_debug_window(layout, half->footprint(), false);
        for (isize g = 0; g < _bound_raster_groups.size() && g < layout.groups.size(); ++g)
            if (auto const* const group = _bound_raster_groups[g]; group != nullptr)
            {
                if (layout.groups[g].resource_root_param >= 0)
                    _list->SetComputeRootDescriptorTable(UINT(layout.groups[g].resource_root_param), group->table_start);
                if (layout.groups[g].sampler_root_param >= 0)
                    _list->SetComputeRootDescriptorTable(UINT(layout.groups[g].sampler_root_param),
                                                         group->sampler_table_start);
            }
        if (layout.inline_constants_root_param >= 0 && !_raster_inline_constants.empty())
            _list->SetComputeRoot32BitConstants(UINT(layout.inline_constants_root_param),
                                                UINT(_raster_inline_constants.size()), _raster_inline_constants.data(),
                                                0);
        bind_software_trace(layout, _bound_raster_groups, false);
        if (layout.dispatch_grid_root_param >= 0 && grid_threads.has_value())
            _list->SetComputeRoot32BitConstants(UINT(layout.dispatch_grid_root_param), 4, grid_threads.value().data(), 0);
        if (layout.mesh_slots_root_param >= 0)
        {
            _list->SetComputeRootUnorderedAccessView(UINT(layout.mesh_slots_root_param), scratch->gpu_virtual_address());
            track_buffer_access(scratch, sg::pipeline_stage_flag::compute, sg::access_flag::shader_write);
        }
        if (layout.task_slots_root_param >= 0)
        {
            auto const writes = layout.mesh_slots_root_param < 0;
            if (writes)
                _list->SetComputeRootUnorderedAccessView(UINT(layout.task_slots_root_param),
                                                         records->gpu_virtual_address());
            else
                _list->SetComputeRootShaderResourceView(UINT(layout.task_slots_root_param),
                                                        records->gpu_virtual_address());
            track_buffer_access(records, sg::pipeline_stage_flag::compute,
                                writes ? sg::access_flag::shader_write : sg::access_flag::shader_read);
        }
        declare_group_accesses(_bound_raster_groups, &half->footprint(), sg::pipeline_stage_flag::compute);
    };
    if (by_records)
    {
        // the task stage's records, then their scan, cut to the slots and the records held, then each launched mesh
        // workgroup
        if (polyfill.task != nullptr)
        {
            bind_half(*polyfill.task, cc::fixed_array<u32, 4>{u32(held), 0, 0, 0});
            if (args == nullptr)
            {
                flush_barriers();
                _list->Dispatch(UINT(grid[0]), UINT(grid[1]), UINT(grid[2]));
            }
            else
            {
                // by the arguments themselves, since a dispatch reads none from the records it writes
                track_buffer_access(args, sg::pipeline_stage_flag::draw_indirect, sg::access_flag::indirect_read);
                flush_barriers();
                _list->ExecuteIndirect(_ctx._dispatch_signature.Get(), 1, args->_resource.Get(),
                                       UINT64(offset_in_bytes), nullptr, 0);
            }
        }
        bind_half(*polyfill.scan, cc::fixed_array<u32, 4>{u32(slots), u32(held), 0, 0});
        flush_barriers();
        _list->Dispatch(1, 1, 1);
        bind_half(polyfill.task != nullptr ? *polyfill.compute : *polyfill.launched, cc::nullopt);
        track_buffer_access(records, sg::pipeline_stage_flag::draw_indirect, sg::access_flag::indirect_read);
        flush_barriers();
        _list->ExecuteIndirect(_ctx._dispatch_signature.Get(), 1, records->_resource.Get(), 2 * 4, nullptr, 0);
    }
    else
    {
        auto const& w = polyfill.workgroup;
        bind_half(*polyfill.compute,
                  cc::fixed_array<u32, 4>{u32(grid[0] * w.x), u32(grid[1] * w.y), u32(grid[2] * w.z), 0});
        flush_barriers();
        _list->Dispatch(UINT(grid[0]), UINT(grid[1]), UINT(grid[2]));
    }

    // The list's compute binding is gone with its pipeline state, as after any raster bind: a dispatch binds again.
    _bound_pipeline_layout = nullptr;
    _bound_groups.clear();
    _bound_footprint = nullptr;
    _bound_footprint_owner = nullptr;

    // The vertex half draws every corner of every slot, in order, under the raster state as it was.
    _list->SetPipelineState(_bound_raster_pso);
    bind_debug_window(*_bound_raster_layout, *_bound_raster_footprint, true);
    CC_ASSERT(_bound_raster_layout->mesh_slots_root_param >= 0, "a mesh polyfill's vertex half reads its slots");
    _list->SetGraphicsRootShaderResourceView(UINT(_bound_raster_layout->mesh_slots_root_param),
                                             scratch->gpu_virtual_address());
    track_buffer_access(scratch, sg::pipeline_stage_flag::vertex, sg::access_flag::shader_read);
    if (records != nullptr)
        track_buffer_access(records, sg::pipeline_stage_flag::draw_indirect, sg::access_flag::indirect_read);
    declare_raster_draw_barriers(false);
    flush_barriers();
    // behind records, as many corners as the scan counted
    if (records != nullptr)
        _list->ExecuteIndirect(_ctx._draw_signature.Get(), 1, records->_resource.Get(), 5 * 4, nullptr, 0);
    else
        _list->DrawInstanced(UINT(groups * polyfill.primitives_per_group * polyfill.corners), 1, 0, 0);
}

void dx12_command_list::raster_draw_indexed(sg::draw_indexed_config const& config)
{
    CC_ASSERT(_in_render_pass, "draw_indexed requires an open rendering scope");
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before drawing");
    CC_ASSERT(_bound_index_buffer != nullptr, "draw_indexed requires a bound index buffer");
    CC_ASSERT(config.index_range.offset >= 0 && config.index_range.size >= 0, "index range must be non-negative");
    CC_ASSERT(config.instance_range.offset >= 0 && config.instance_range.size >= 0, "instance range must be "
                                                                                    "non-negative");

    // **An index fetch starts on a 4-byte boundary**, and `index_range.offset` counts indices rather than bytes — so
    // an aligned view is not enough on its own.
    // Metal is the backend that cannot do otherwise, and it answers a misaligned fetch by drawing part of the mesh
    // with no error and no validation message; the rule is sg's so that it fails here too.
    CC_ASSERT(sg::is_aligned_index_fetch(_index_format, _index_view_offset_in_bytes, config.index_range.offset),
              "an odd first index into a 16-bit index buffer starts the fetch off a 4-byte boundary. Use an even "
              "first index, or 32-bit indices — sg::is_aligned_index_fetch answers it without asserting");

    declare_raster_draw_barriers(true);
    flush_barriers();
    _list->DrawIndexedInstanced(UINT(config.index_range.size), UINT(config.instance_range.size),
                                UINT(config.index_range.offset), config.vertex_offset,
                                UINT(config.instance_range.offset));
}

void dx12_command_list::raster_draw_indexed_indirect(sg::raw_buffer_handle const& args, isize offset_in_bytes, isize count)
{
    CC_ASSERT(_in_render_pass, "an indirect draw requires an open rendering scope");
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before drawing");
    CC_ASSERT(_bound_index_buffer != nullptr, "an indirect indexed draw requires a bound index buffer");
    auto const buffer = std::dynamic_pointer_cast<dx12_buffer const>(args);
    CC_ASSERT(buffer != nullptr, "indirect draw arguments are not a dx12 buffer");
    CC_ASSERT(!buffer->is_expired(), "indirect draw arguments are a transient buffer used past its epoch");

    // The arguments are read by the command processor, a stage of their own, before any vertex is fetched.
    track_buffer_access(buffer, sg::pipeline_stage_flag::draw_indirect, sg::access_flag::indirect_read);
    declare_raster_draw_barriers(true);
    flush_barriers();
    _list->ExecuteIndirect(_ctx._draw_indexed_signature.Get(), UINT(count), buffer->_resource.Get(),
                           UINT64(offset_in_bytes), nullptr, 0);
}

void dx12_command_list::raster_draw_mesh_tasks_indirect(sg::raw_buffer_handle const& args,
                                                        isize offset_in_bytes,
                                                        isize count)
{
    CC_ASSERT(_in_render_pass, "an indirect mesh draw requires an open rendering scope");
    CC_ASSERT(_bound_raster_layout != nullptr, "bind a raster pipeline before drawing");
    CC_ASSERT(_ctx._draw_mesh_signature != nullptr, "an indirect mesh draw needs a device with mesh shaders");
    auto const buffer = std::dynamic_pointer_cast<dx12_buffer const>(args);
    CC_ASSERT(buffer != nullptr, "indirect mesh draw arguments are not a dx12 buffer");
    CC_ASSERT(!buffer->is_expired(), "indirect mesh draw arguments are a transient buffer used past its epoch");

    track_buffer_access(buffer, sg::pipeline_stage_flag::draw_indirect, sg::access_flag::indirect_read);
    declare_raster_draw_barriers(false);
    flush_barriers();
    _list->ExecuteIndirect(_ctx._draw_mesh_signature.Get(), UINT(count), buffer->_resource.Get(),
                           UINT64(offset_in_bytes), nullptr, 0);
}
} // namespace sg::backend::dx12
