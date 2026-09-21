// Compute recording for the webgpu backend, and the state replay both pass kinds share.

#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <clean-core/common/utility.hh>
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
        state.constants = cc::vector<byte>::create_filled(layout->inline_constants_bytes(), byte(0));
        state.constants_dirty = layout->inline_constants_bytes() > 0;
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
    if (state.layout == nullptr || state.layout->inline_constants_bytes() == 0)
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
        end_open_pass();
    if (_compute_pass)
        return;
    auto const desc
        = WGPUComputePassDescriptor{.nextInChain = nullptr, .label = to_wgpu("sg compute"), .timestampWrites = nullptr};
    _compute_pass = wgpu_compute_pass(wgpuCommandEncoderBeginComputePass(encoder(), &desc));
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
        auto const has_constants = s.layout->inline_constants_bytes() > 0;
        auto const offset = s.constants_offset;
        wgpuComputePassEncoderSetBindGroup(pass, u32(sg::reserved_binding_group),
                                           s.layout->reserved_group_for(s.constants_page), has_constants ? 1 : 0,
                                           has_constants ? &offset : nullptr);
    }
    s.needs_full_apply = false;
}

void webgpu_command_list::compute_bind_pipeline(sg::compute_pipeline const& pipeline)
{
    auto const* wp = dynamic_cast<webgpu_compute_pipeline const*>(&pipeline);
    CC_ASSERT(wp != nullptr, "compute_pipeline is not a webgpu compute_pipeline");
    rebind_layout(_compute, wp->layout.get());
    _compute.compute_pipeline = wp->pipeline;
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

void webgpu_command_list::compute_declare_array_buffer_access(cc::string_view,
                                                              cc::span<sg::array_buffer_access const> elements)
{
    // No layout accepts an array binding here, so the only declaration that can be right is the empty one.
    CC_ASSERT(elements.empty(), "webgpu has no binding arrays, so there is no array element to declare");
}

void webgpu_command_list::compute_declare_array_texture_access(cc::string_view,
                                                               cc::span<sg::array_texture_access const> elements)
{
    CC_ASSERT(elements.empty(), "webgpu has no binding arrays, so there is no array element to declare");
}

void webgpu_command_list::compute_dispatch(int x, int y, int z)
{
    CC_ASSERT(x >= 0 && y >= 0 && z >= 0, "dispatch group counts must be non-negative");
    // The device is requested with the default limits, so this is exactly its maxComputeWorkgroupsPerDimension.
    constexpr int max_groups_per_dimension = 65535;
    CC_ASSERTF(x <= max_groups_per_dimension && y <= max_groups_per_dimension && z <= max_groups_per_dimension,
               "dispatch of ({}, {}, {}) groups exceeds webgpu's {} per dimension", x, y, z, max_groups_per_dimension);
    CC_ASSERT(!_in_rendering_scope, "dispatch must not be recorded inside a rendering scope; close the scope first");
    open_compute_pass();
    apply_compute_state();
    wgpuComputePassEncoderDispatchWorkgroups(compute_pass(), u32(x), u32(y), u32(z));
}

// -- raster state helpers shared with webgpu_command_list.raster.cc --

void webgpu_command_list::raster_bind_group(int group_index, sg::binding_group const& group)
{
    CC_ASSERT(_in_rendering_scope, "raster bind_group is only valid inside a rendering scope");
    bind_group_into(_raster, _ctx, group_index, group);
    touch_group(group);
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
    rebind_layout(_raster, wp->layout.get());
    _raster.render_pipeline = wp->pipeline;
    _keep_alive.push_back(wp->layout);
}
} // namespace sg::backend::webgpu
