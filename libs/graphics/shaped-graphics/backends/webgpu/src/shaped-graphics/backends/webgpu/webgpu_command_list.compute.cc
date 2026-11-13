// Compute recording for the webgpu backend, and the state replay both pass kinds share.

#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backends/webgpu/webgpu_binding_group.hh>
#include <shaped-graphics/backends/webgpu/webgpu_buffer.hh>
#include <shaped-graphics/backends/webgpu/webgpu_context.hh>

namespace sg::backend::webgpu
{
namespace
{
/// Resets `state` for a pipeline over `layout`, keeping the constants and the groups when the layout is the same one.
void rebind_layout(webgpu_command_list::bound_state& state, webgpu_pipeline_layout const* layout)
{
    if (state.layout != layout)
    {
        state.layout = layout;
        state.groups.clear();
        for (isize i = 0; i < layout->groups().size(); ++i)
            state.groups.push_back({});
        state.constants = cc::vector<byte>::create_filled(layout->reserved_block_bytes(), byte(0));
        state.constants_dirty = layout->reserved_block_bytes() > 0;
        state.constants_page = nullptr;
    }
    state.needs_full_apply = true;
}

void bind_group_into(webgpu_command_list::bound_state& state,
                     webgpu_context const& ctx,
                     int group_index,
                     sg::binding_group const& group)
{
    CC_ASSERT(state.layout != nullptr, "bind a pipeline before binding groups");
    CC_ASSERT(group_index >= 0 && group_index < int(state.groups.size()), "binding-group slot out of range for the "
                                                                          "bound pipeline layout");
    auto const* wg = dynamic_cast<webgpu_binding_group const*>(&group);
    CC_ASSERT(wg != nullptr, "binding_group is not a webgpu binding_group");
    CC_ASSERT(!(wg->transient && wg->creation_epoch != ctx.current_epoch()), "transient binding_group used past its "
                                                                             "epoch");
    CC_ASSERTF(
        wg->layout == state.layout->groups()[group_index], "{}",
        sg::impl::describe_layout_mismatch(group_index, state.layout->groups()[group_index].get(), wg->layout.get()));
    auto const pinned = wg->layout->group_index();
    CC_ASSERTF(!pinned.has_value() || pinned.value() == u32(group_index),
               "binding_group is pinned to group index {} by its bindings and cannot be bound at slot {}",
               pinned.value_or(0), group_index);
    state.groups[group_index] = wg->_group;
    state.needs_full_apply = true;

    // The group's roots join the reserved block, at the group's place among the pipeline's acceleration members.
    if (!wg->acceleration_roots.empty())
    {
        auto const at = state.layout->roots_offset_in_block() + state.layout->acceleration_base(group_index) * 4;
        cc::memcpy(state.constants.data() + at, wg->acceleration_roots.data(),
                   size_t(wg->acceleration_roots.size()) * sizeof(u32));
        state.constants_dirty = true;
    }
}

void set_constants_into(webgpu_command_list::bound_state& state, cc::span<byte const> data, cc::optional<isize> offset)
{
    CC_ASSERT(state.layout != nullptr, "bind a pipeline before setting inline constants");
    auto const block = state.layout->inline_constants_bytes();
    CC_ASSERT(block > 0, "the bound pipeline layout declares no inline_constants block");
    CC_ASSERT(data.size() % 4 == 0, "inline-constants payload size must be a multiple of 4 bytes");

    auto const off = offset.value_or(0);
    CC_ASSERT(off >= 0 && off % 4 == 0, "inline-constants offset must be non-negative and a multiple of 4");
    if (offset.has_value())
        CC_ASSERT(off + data.size() <= block, "partial inline-constants update exceeds the declared block size");
    else
        CC_ASSERT(data.size() == block, "full inline-constants replace must match the declared block size");

    cc::memcpy(state.constants.data() + off, data.data(), size_t(data.size()));
    state.constants_dirty = true;
    state.needs_full_apply = true;
}
} // namespace

void webgpu_command_list::place_constants(bound_state& state)
{
    if (state.layout == nullptr || state.layout->reserved_block_bytes() == 0)
        return;
    if (!state.constants_dirty && state.constants_page != nullptr)
        return;
    auto const placement = _ctx._constant_pages.place(_constant_pages, state.constants);
    state.constants_page = placement.page;
    state.constants_offset = placement.offset;
    state.constants_dirty = false;
}

void webgpu_command_list::open_compute_pass()
{
    if (_render_pass)
        end_open_pass("a dispatch");
    if (_compute_pass)
        return;
    auto writes = WGPUPassTimestampWrites{};
    auto const desc = WGPUComputePassDescriptor{.nextInChain = nullptr,
                                                .label = to_wgpu("sg compute"),
                                                .timestampWrites = pass_timestamp_writes(writes)};
    _compute_pass = wgpu_compute_pass(wgpuCommandEncoderBeginComputePass(_encoder.get(), &desc));
    _compute.needs_full_apply = true;
}

void webgpu_command_list::apply_compute_state()
{
    auto& s = _compute;
    CC_ASSERT(bool(s.compute_pipeline), "bind a compute pipeline before dispatching");

    // Placed before the early-out: a changed block always marks the state dirty, so this only ever places once.
    place_constants(s);
    if (!s.needs_full_apply)
        return;

    auto const pass = compute_pass();
    wgpuComputePassEncoderSetPipeline(pass, s.compute_pipeline.get());
    for (isize i = 0; i < s.groups.size(); ++i)
        if (s.groups[i])
            wgpuComputePassEncoderSetBindGroup(pass, u32(i), s.groups[i].get(), 0, nullptr);
    if (s.layout->has_reserved_group())
    {
        for (auto i = s.groups.size(); i < sg::reserved_binding_group; ++i)
            wgpuComputePassEncoderSetBindGroup(pass, u32(i), s.layout->empty_group(), 0, nullptr);
        u32 offsets[3] = {};
        auto const offset_count = s.layout->reserved_dynamic_offsets(s.constants_offset, offsets);
        auto window = sg::raw_buffer_handle();
        if (sg::impl::debug_windows::k_enabled)
        {
            window = _ctx.debug_windows().window_for_recording(s.writes_debug);
            touch(window);
        }
        wgpuComputePassEncoderSetBindGroup(
            pass, u32(sg::reserved_binding_group),
            s.layout->reserved_group_for(
                s.constants_page, window != nullptr ? static_cast<webgpu_buffer const&>(*window).raw() : nullptr),
            size_t(offset_count), offset_count > 0 ? offsets : nullptr);
    }
    s.needs_full_apply = false;
}

void webgpu_command_list::compute_bind_pipeline(sg::compute_pipeline const& pipeline)
{
    auto const* wp = dynamic_cast<webgpu_compute_pipeline const*>(&pipeline);
    CC_ASSERT(wp != nullptr, "compute_pipeline is not a webgpu compute_pipeline");
    rebind_layout(_compute, wp->layout.get());
    _compute.compute_pipeline = wp->pipeline;
    _compute.writes_debug = !pipeline.footprint().debug_stages().is_empty();
    _keep_alive.push_back(wp->layout);
}

void webgpu_command_list::compute_bind_group(int group_index, sg::binding_group const& group)
{
    bind_group_into(_compute, _ctx, group_index, group);
    touch_group(group);
}

void webgpu_command_list::touch_group(sg::binding_group const& group)
{
    auto const& wg = static_cast<webgpu_binding_group const&>(group);
    for (auto const& b : wg.referenced_buffers)
        touch(b);
    for (auto const& t : wg.referenced_textures)
        touch(t);
}

void webgpu_command_list::compute_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset)
{
    set_constants_into(_compute, data, offset);
}

u32 webgpu_command_list::compute_declare_array_buffer_access(cc::string_view,
                                                             cc::span<sg::array_buffer_access const> elements)
{
    // No layout accepts an array binding here, so the only declaration that can be right is the empty one.
    // It still gets an id, so the value holding it works as it does everywhere else.
    CC_ASSERT(elements.empty(), "webgpu has no binding arrays, so there is no array element to declare");
    return next_array_declaration_id();
}

u32 webgpu_command_list::compute_declare_array_texture_access(cc::string_view,
                                                              cc::span<sg::array_texture_access const> elements)
{
    CC_ASSERT(elements.empty(), "webgpu has no binding arrays, so there is no array element to declare");
    return next_array_declaration_id();
}

void webgpu_command_list::withdraw_array_declaration(u32)
{
    // Nothing was kept: with no array to meet, there is nothing a declaration could change or have missed.
}

void webgpu_command_list::compute_dispatch(int x, int y, int z)
{
    CC_ASSERT(x >= 0 && y >= 0 && z >= 0, "dispatch group counts must be non-negative");
    // The device is requested with the default limits, so this is exactly its maxComputeWorkgroupsPerDimension.
    CC_ASSERTF(x <= sg::max_dispatch_groups_per_dimension && y <= sg::max_dispatch_groups_per_dimension
                   && z <= sg::max_dispatch_groups_per_dimension,
               "dispatch of ({}, {}, {}) groups exceeds the {} per dimension every backend allows", x, y, z,
               sg::max_dispatch_groups_per_dimension);
    CC_ASSERT(!_in_rendering_scope, "dispatch must not be recorded inside a rendering scope; close the scope first");
    bring_pool_writes_forward();
    // the threads dispatch_threads asked for, which a pipeline dispatched by threads alone returns past
    if (_compute.layout != nullptr && _compute.layout->has_dispatch_grid())
    {
        u32 const grid[4] = {u32(_dispatch_grid[0]), u32(_dispatch_grid[1]), u32(_dispatch_grid[2]), 0};
        auto* const at = _compute.constants.data() + _compute.layout->grid_offset_in_block();
        if (cc::memcmp(at, grid, sizeof(grid)) != 0)
        {
            cc::memcpy(at, grid, sizeof(grid));
            _compute.constants_dirty = true;
            _compute.needs_full_apply = true;
        }
    }
    open_compute_pass();
    apply_compute_state();
    wgpuComputePassEncoderDispatchWorkgroups(compute_pass(), u32(x), u32(y), u32(z));
}

void webgpu_command_list::compute_dispatch_indirect(sg::raw_buffer_handle const& args, isize offset_in_bytes)
{
    CC_ASSERT(!_in_rendering_scope, "dispatch must not be recorded inside a rendering scope; close the scope first");
    auto const* buffer = dynamic_cast<webgpu_buffer const*>(args.get());
    CC_ASSERT(buffer != nullptr, "indirect dispatch arguments are not a webgpu buffer");
    CC_ASSERT(!buffer->is_expired(), "indirect dispatch arguments are a transient buffer used past its epoch");

    // WebGPU orders the arguments' reads after earlier writes, including an earlier dispatch in this pass; the list only keeps the buffer alive.
    touch(args);
    bring_pool_writes_forward();
    open_compute_pass();
    apply_compute_state();
    wgpuComputePassEncoderDispatchWorkgroupsIndirect(compute_pass(), buffer->raw(), u64(offset_in_bytes));
}

// -- a polyfilled mesh draw: the compute half in a pass of its own, then a draw of what it wrote --

void webgpu_command_list::raster_draw_mesh_polyfilled(sg::impl::mesh_polyfill_state const& polyfill, int x, int y, int z)
{
    CC_ASSERTF(x <= sg::max_dispatch_groups_per_dimension && y <= sg::max_dispatch_groups_per_dimension
                   && z <= sg::max_dispatch_groups_per_dimension,
               "a mesh draw of ({}, {}, {}) workgroups exceeds the {} per dimension every backend dispatches", x, y, z,
               sg::max_dispatch_groups_per_dimension);
    draw_mesh_polyfilled(polyfill, {x, y, z}, nullptr, 0);
}

void webgpu_command_list::raster_draw_mesh_polyfilled_indirect(sg::impl::mesh_polyfill_state const& polyfill,
                                                               sg::raw_buffer_handle const& args,
                                                               isize offset_in_bytes,
                                                               isize count)
{
    auto const* const buffer = dynamic_cast<webgpu_buffer const*>(args.get());
    CC_ASSERT(buffer != nullptr, "indirect mesh draw arguments are not a webgpu buffer");
    CC_ASSERT(!buffer->is_expired(), "indirect mesh draw arguments are a transient buffer used past its epoch");
    touch(args);
    for (auto i = isize(0); i < count; ++i)
        draw_mesh_polyfilled(polyfill, {0, 0, 0}, buffer, offset_in_bytes + i * isize(sizeof(sg::dispatch_args)));
}

void webgpu_command_list::draw_mesh_polyfilled(sg::impl::mesh_polyfill_state const& polyfill,
                                               cc::fixed_array<int, 3> grid,
                                               webgpu_buffer const* args,
                                               isize offset_in_bytes)
{
    CC_ASSERT(_in_rendering_scope, "a mesh draw is only valid inside a rendering scope");
    // records launch the mesh workgroups behind a task stage, and on an indirect draw, whose counts are the GPU's
    auto const by_records = polyfill.task != nullptr || args != nullptr;
    auto const groups = i64(grid[0]) * i64(grid[1]) * i64(grid[2]);
    auto const slots = !by_records     ? groups
                     : args != nullptr ? i64(polyfill.indirect_capacity())
                                       : i64(polyfill.task_capacity(isize(groups)));
    auto const records = polyfill.task == nullptr ? i64(1)
                       : args != nullptr          ? i64(polyfill.indirect_capacity())
                                                  : groups;
    (void)_ctx.mesh_scratch_for(isize(slots * polyfill.words_per_group * 4));
    auto const task_records
        = by_records ? _ctx.task_scratch_for(isize((12 + records * polyfill.record_words) * 4)) : WGPUBuffer(nullptr);
    _keep_alive.push_back(polyfill.compute);
    _keep_alive.push_back(polyfill.task);
    _keep_alive.push_back(polyfill.launched);
    _keep_alive.push_back(polyfill.scan);
    bring_pool_writes_forward();

    // Each compute form sees what the draw would: the raster state's groups and its reserved block, which the compute
    // twin's layout holds at the same offsets, the grid behind them where it reads one.
    auto const half_of = [&](sg::compute_pipeline const& pipeline, cc::optional<cc::fixed_array<u32, 4>> grid_threads)
    {
        auto const* const p = dynamic_cast<webgpu_compute_pipeline const*>(&pipeline);
        CC_ASSERT(p != nullptr, "a polyfill's compute form is not a webgpu compute_pipeline");
        auto half = bound_state();
        rebind_layout(half, p->layout.get());
        half.compute_pipeline = p->pipeline;
        half.writes_debug = !p->footprint().debug_stages().is_empty();
        for (isize i = 0; i < half.groups.size() && i < _raster.groups.size(); ++i)
            half.groups[i] = _raster.groups[i];
        cc::memcpy(half.constants.data(), _raster.constants.data(),
                   size_t(cc::min(half.constants.size(), _raster.constants.size())));
        if (grid_threads.has_value())
            cc::memcpy(half.constants.data() + p->layout->grid_offset_in_block(), grid_threads.value().data(), 16);
        half.constants_dirty = true;
        _keep_alive.push_back(p->layout);
        return half;
    };

    end_open_pass("a polyfilled mesh draw", split_remedy::none);
    // The header's task grid, and an indirect draw's one record with no task stage, before any pass reads them.
    auto const upload = [&](cc::span<u32 const> words, isize word)
    {
        auto const span = stage_upload(cc::as_bytes(words), words.size() * 4);
        wgpuCommandEncoderCopyBufferToBuffer(encoder(), span.buffer, u64(span.offset), task_records, u64(word * 4),
                                             u64(words.size() * 4));
        if (span.overflow)
            _keep_alive.push_back(std::make_shared<wgpu_buffer>(span.overflow));
    };
    auto const copy_args = [&](isize word)
    {
        wgpuCommandEncoderCopyBufferToBuffer(encoder(), args->raw(), u64(offset_in_bytes), task_records, u64(word * 4),
                                             u64(sizeof(sg::dispatch_args)));
    };
    if (by_records && args == nullptr)
    {
        u32 const task_grid[] = {u32(grid[0]), u32(grid[1]), u32(grid[2])};
        upload(task_grid, 9);
    }
    else if (polyfill.task != nullptr)
        copy_args(9);
    else if (by_records)
    {
        u32 const one[] = {1, 1, 1};
        upload(one, 9);
        copy_args(12);
    }

    auto kept = cc::move(_compute);
    open_compute_pass();
    if (by_records)
    {
        // the task stage's records, then their scan, cut to the slots and the records held, then each launched mesh
        // workgroup; WebGPU orders each dispatch of a pass after the one before it
        if (polyfill.task != nullptr)
        {
            _compute = half_of(*polyfill.task, cc::fixed_array<u32, 4>{u32(records), 0, 0, 0});
            apply_compute_state();
            if (args == nullptr)
                wgpuComputePassEncoderDispatchWorkgroups(compute_pass(), u32(grid[0]), u32(grid[1]), u32(grid[2]));
            else // by the arguments themselves, since a dispatch reads none from the records it writes
                wgpuComputePassEncoderDispatchWorkgroupsIndirect(compute_pass(), args->raw(), u64(offset_in_bytes));
        }
        _compute = half_of(*polyfill.scan, cc::fixed_array<u32, 4>{u32(slots), u32(records), 0, 0});
        apply_compute_state();
        wgpuComputePassEncoderDispatchWorkgroups(compute_pass(), 1, 1, 1);
        _compute = half_of(polyfill.task != nullptr ? *polyfill.compute : *polyfill.launched, cc::nullopt);
        apply_compute_state();
        wgpuComputePassEncoderDispatchWorkgroupsIndirect(compute_pass(), task_records, 2 * 4);
    }
    else
    {
        auto const& w = polyfill.workgroup;
        _compute = half_of(*polyfill.compute,
                           cc::fixed_array<u32, 4>{u32(grid[0] * w.x), u32(grid[1] * w.y), u32(grid[2] * w.z), 0});
        apply_compute_state();
        wgpuComputePassEncoderDispatchWorkgroups(compute_pass(), u32(grid[0]), u32(grid[1]), u32(grid[2]));
    }
    end_open_pass("a polyfilled mesh draw", split_remedy::none);
    _compute = cc::move(kept);
    _compute.needs_full_apply = true;

    // The vertex half reopens the pass, loading what it held, and draws every corner of every slot in order; behind
    // records, as many as the scan counted.
    order_draw_within_pass(false);
    apply_raster_state();
    if (by_records)
        wgpuRenderPassEncoderDrawIndirect(render_pass(), task_records, 5 * 4);
    else
        wgpuRenderPassEncoderDraw(render_pass(), u32(groups * polyfill.primitives_per_group * polyfill.corners), 1, 0, 0);
}

// -- raster state helpers shared with webgpu_command_list.raster.cc --

void webgpu_command_list::raster_bind_group(int group_index, sg::binding_group const& group)
{
    CC_ASSERT(_in_rendering_scope, "raster bind_group is only valid inside a rendering scope");
    bind_group_into(_raster, _ctx, group_index, group);
    touch_group(group);
    _keep_alive.push_back(group.weak_from_this().lock());
    while (int(_raster_group_objects.size()) <= group_index)
        _raster_group_objects.push_back(nullptr);
    _raster_group_objects[group_index] = static_cast<webgpu_binding_group const*>(&group);
}

void webgpu_command_list::raster_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset)
{
    CC_ASSERT(_in_rendering_scope, "raster inline constants are only valid inside a rendering scope");
    set_constants_into(_raster, data, offset);
}

void webgpu_command_list::raster_bind_pipeline(sg::raster_pipeline const& pipeline)
{
    CC_ASSERT(_in_rendering_scope, "raster bind_pipeline is only valid inside a rendering scope");
    auto const* wp = dynamic_cast<webgpu_raster_pipeline const*>(&pipeline);
    CC_ASSERT(wp != nullptr, "raster_pipeline is not a webgpu raster_pipeline");
    // A new layout unbinds every group, and what a draw orders against goes with them.
    if (_raster.layout != wp->layout.get())
        _raster_group_objects.clear();
    rebind_layout(_raster, wp->layout.get());
    _raster.render_pipeline = wp->pipeline;
    _keep_alive.push_back(wp->layout);
    _keep_alive.push_back(pipeline.weak_from_this().lock());
    _raster_footprint = &pipeline.footprint();
    _raster.writes_debug = !pipeline.footprint().debug_stages().is_empty();
}
} // namespace sg::backend::webgpu
