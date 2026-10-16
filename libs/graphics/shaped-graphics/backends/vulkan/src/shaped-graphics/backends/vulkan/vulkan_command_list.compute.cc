// Compute recording for the vulkan backend: pipeline and group binding, the array-access declarations, and dispatch.
// The lifecycle and the transfer paths live in vulkan_command_list.cc.

#include <clean-core/common/assert.hh>
#include <clean-core/common/assertf.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_command_list.hh>
#include <shaped-graphics/backends/vulkan/vulkan_compute_pipeline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>
#include <shaped-graphics/barrier/access_inference.hh>

namespace sg::backend::vulkan
{
void vulkan_command_list::compute_bind_pipeline(sg::compute_pipeline const& pipeline)
{
    keep_bound(pipeline.weak_from_this().lock());
    auto const* vp = dynamic_cast<vulkan_compute_pipeline const*>(&pipeline);
    CC_ASSERT(vp != nullptr, "compute_pipeline is not a vulkan compute_pipeline");

    // The descriptor buffer has to be bound before any offset into it is set.
    // One buffer, where dx12 needs two heaps: Vulkan mixes samplers and resources in one set layout, so there is no
    // sampler heap to keep in step.
    auto const binding = VkDescriptorBufferBindingInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
        .address = _ctx._descriptor_heap.device_address(),
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
    };
    _ctx._descriptor_functions.cmd_bind_descriptor_buffers(_buffer, 1, &binding);
    vkCmdBindPipeline(_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vp->_pipeline);

    // A new pipeline may declare a different number of slots, so the bound groups reset to one null per slot.
    _bound_pipeline_layout = vp->layout.get();
    _bound_pipeline_layout->bind_embedded_samplers(_buffer, VK_PIPELINE_BIND_POINT_COMPUTE);
    _bound_groups.clear_resize_to_filled(_bound_pipeline_layout->_groups.size(), nullptr);
    _bound_footprint = &pipeline.footprint();
    _bound_footprint_owner = &pipeline;
}

void vulkan_command_list::compute_bind_group(int group_index, sg::binding_group const& group)
{
    keep_bound(group.weak_from_this().lock());
    CC_ASSERT(_bound_pipeline_layout != nullptr, "bind a compute pipeline before binding groups");
    CC_ASSERT(group_index >= 0 && group_index < int(_bound_groups.size()), "binding-group slot out of range for the "
                                                                           "bound pipeline layout");

    auto const* vg = dynamic_cast<vulkan_binding_group const*>(&group);
    CC_ASSERT(vg != nullptr, "binding_group is not a vulkan binding_group");

    // Transient expiry tripwire: a transient group's bytes are recycled after its epoch, so binding one past that
    // epoch would point the set at another epoch's descriptors.
    CC_ASSERT(!(vg->transient && vg->creation_epoch != _ctx.current_epoch()),
              "transient binding_group used past its epoch (its descriptors have been recycled)");

    // The group's schema must be the one the pipeline layout declared at this slot, or the offset below addresses a
    // set with a different shape.
    CC_ASSERTF(vg->layout == _bound_pipeline_layout->_groups[group_index], "{}",
               sg::impl::describe_layout_mismatch(group_index, _bound_pipeline_layout->_groups[group_index].get(),
                                                  vg->layout.get()));
    auto const pinned = vg->layout->group_index();
    CC_ASSERTF(!pinned.has_value() || pinned.value() == u32(group_index),
               "binding_group is pinned to group index {} by its bindings and cannot be bound at slot {}",
               pinned.value_or(0), group_index);

    // Remember the bound group so its views' accesses are declared at dispatch, the point work runs.
    _bound_groups[group_index] = vg;

    // Binding a group is naming an offset into the one bound descriptor buffer.
    // The slot is `firstSet` — the direct analogue of dx12's root-parameter index, and rather more obvious about it.
    u32 const buffer_index = 0;
    auto const offset = VkDeviceSize(vg->range.offset);
    _ctx._descriptor_functions.cmd_set_descriptor_offsets(_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                                          _bound_pipeline_layout->_layout, u32(group_index), 1,
                                                          &buffer_index, &offset);
}

void vulkan_command_list::compute_set_inline_constants(cc::span<byte const> data, cc::optional<isize> offset)
{
    CC_ASSERT(_bound_pipeline_layout != nullptr, "bind a compute pipeline before setting inline constants");
    CC_ASSERT(_bound_pipeline_layout->_inline_constants_bytes > 0, "the bound pipeline layout declares no "
                                                                   "inline_constants block");
    CC_ASSERT(data.size() % 4 == 0, "inline-constants payload size must be a multiple of 4 bytes");

    isize const off = offset.value_or(0);
    CC_ASSERT(off >= 0 && off % 4 == 0, "inline-constants offset must be non-negative and a multiple of 4");
    if (offset.has_value())
        CC_ASSERT(off + data.size() <= isize(_bound_pipeline_layout->_inline_constants_bytes),
                  "partial inline-constants update exceeds the declared block size");
    else
        CC_ASSERT(data.size() == isize(_bound_pipeline_layout->_inline_constants_bytes),
                  "full inline-constants replace must match the declared block size");

    vkCmdPushConstants(_buffer, _bound_pipeline_layout->_layout, VK_SHADER_STAGE_ALL, u32(off), u32(data.size()),
                       data.data());
}

u32 vulkan_command_list::compute_declare_array_buffer_access(cc::string_view binding_name,
                                                             cc::span<sg::array_buffer_access const> elements)
{
    CC_ASSERT(!binding_name.empty(), "declare_array_buffer_access requires a binding name");

    // Arrays / bindless are not auto-tracked: which elements a shader indexes, and how, cannot be inferred, so the
    // caller declares it here.
    // In force for every dispatch and trace until withdrawn, met by whichever bound group holds the array at each.
    auto const id = next_array_declaration_id();
    _compute_arrays.declare_buffers(id, binding_name, elements);
    return id;
}

u32 vulkan_command_list::compute_declare_array_texture_access(cc::string_view binding_name,
                                                              cc::span<sg::array_texture_access const> elements)
{
    CC_ASSERT(!binding_name.empty(), "declare_array_texture_access requires a binding name");
    auto const id = next_array_declaration_id();
    _compute_arrays.declare_textures(id, binding_name, elements);
    return id;
}

void vulkan_command_list::withdraw_array_declaration(u32 id)
{
    if (!_compute_arrays.withdraw(id))
        (void)_raster_arrays.withdraw(id);
}

void vulkan_command_list::declare_group_accesses(cc::span<vulkan_binding_group const* const> groups,
                                                 sg::impl::pipeline_footprint const* footprint,
                                                 sg::pipeline_stage_flags op_stages)
{
    for (auto group = 0; group < int(groups.size()); ++group)
    {
        auto const* bound_group = groups[group];
        if (bound_group == nullptr)
            continue;

        for (auto const& view : bound_group->hazard_views)
            if (view.buffer != nullptr)
                if (auto const a = sg::access_at(footprint, group, view.binding, view.bound_as, op_stages); a.has_value())
                    track_buffer_access(*view.buffer, a.value().stages, a.value().access);

        // A bound texture also moves to the layout its view class needs, a sampled one to shader_texture and a
        // storage one to shader_image, but only where the code touches it at all.
        for (auto const& tv : bound_group->texture_hazard_views)
            if (auto const a = sg::access_at(footprint, group, tv.binding, tv.bound_as, op_stages); a.has_value())
                (void)track_texture_access(*tv.texture, tv.range, a.value().stages, a.value().access,
                                           sg::shader_layout_of(tv.bound_as));
    }
}

void vulkan_command_list::declare_array_accesses(cc::span<vulkan_binding_group const* const> groups,
                                                 sg::impl::pipeline_footprint const* footprint,
                                                 void const* pipeline,
                                                 sg::pipeline_stage_flags op_stages,
                                                 sg::impl::array_declaration_book& book)
{
    // Which elements of an array a shader indexes cannot be inferred, so the caller declares them, for as long as it likes.
    // Every bound group holding an array meets the declarations naming it; a group is kept alive by keep_bound, so its
    // address is its identity for the whole list.
    _array_sites.clear();
    for (auto group = 0; group < int(groups.size()); ++group)
        if (groups[group] != nullptr)
            for (auto const& ab : groups[group]->array_bindings)
                _array_sites.push_back({.group = groups[group], .group_index = group, .array = &ab});
    if (_array_sites.empty())
        return;

    book.apply(
        cc::span<sg::impl::array_site<vulkan_array_binding> const>(_array_sites), footprint, pipeline, op_stages,
        _member_writes, _stats,
        [&](vulkan_array_element const& element, sg::pipeline_stage_flags stages, sg::access_flags access)
        { return track_buffer_access(*element.buffer, stages, access, true); },
        [&](vulkan_array_element const& element, sg::pipeline_stage_flags stages, sg::access_flags access,
            sg::texture_layout layout)
        { return track_texture(*element.texture, element.range, stages, access, layout, true).found; },
        [&](vulkan_array_binding const& ab, vulkan_array_element const& element, sg::impl::array_plan const& plan)
        {
            if (element.buffer != nullptr)
            {
                // One memory barrier for the buffers rather than one per element.
                track_buffer_access(*element.buffer, plan.stages, plan.cover_access);
                _global_barrier_buffers.insert(element.buffer.get());
            }
            else
            {
                // A memory barrier moves no layout, so a texture still gets its own transition where it needs one.
                (void)track_texture_access(*element.texture, element.range, plan.stages, plan.cover_access,
                                           sg::shader_layout_of(ab.bound_as));
            }
        });
}

void vulkan_command_list::compute_dispatch(int x, int y, int z)
{
    CC_ASSERT(x >= 0 && y >= 0 && z >= 0, "dispatch group counts must be non-negative");
    CC_ASSERTF(x <= sg::max_dispatch_groups_per_dimension && y <= sg::max_dispatch_groups_per_dimension
                   && z <= sg::max_dispatch_groups_per_dimension,
               "dispatch of ({}, {}, {}) groups exceeds the {} per dimension every backend allows", x, y, z,
               sg::max_dispatch_groups_per_dimension);
    CC_ASSERT(!_in_render_pass, "dispatch must not be recorded inside a rendering scope; close the scope first");

    // Declare each bound group's shader accesses before the dispatch: the tracker emits any hazard barrier (a prior
    // copy_write → shader_read RAW, a WAW between two dispatches), and — unlike dx12 — carries the access across
    // command lists, since Vulkan has no state decay to ride on.
    declare_group_accesses(_bound_groups, _bound_footprint, sg::pipeline_stage_flag::compute);

    // Array bindings are not auto-tracked — apply (and account for) the caller's explicit declarations.
    declare_array_accesses(_bound_groups, _bound_footprint, _bound_footprint_owner, sg::pipeline_stage_flag::compute,
                           _compute_arrays);

    // Emit every hazard the bound resources declared, batched, right before the dispatch consumes them.
    flush_barriers();
    vkCmdDispatch(_buffer, u32(x), u32(y), u32(z));
}

void vulkan_command_list::compute_dispatch_indirect(sg::raw_buffer_handle const& args, isize offset_in_bytes)
{
    CC_ASSERT(!_in_render_pass, "dispatch must not be recorded inside a rendering scope; close the scope first");
    auto const buffer = std::dynamic_pointer_cast<vulkan_buffer const>(args);
    CC_ASSERT(buffer != nullptr, "indirect dispatch arguments are not a vulkan buffer");
    CC_ASSERT(!buffer->is_expired(), "indirect dispatch arguments are a transient buffer used past its epoch");

    // The arguments are fetched at DRAW_INDIRECT, before the dispatch's shaders run.
    track_buffer_access(*buffer, sg::pipeline_stage_flag::draw_indirect, sg::access_flag::indirect_read);
    declare_group_accesses(_bound_groups, _bound_footprint, sg::pipeline_stage_flag::compute);
    declare_array_accesses(_bound_groups, _bound_footprint, _bound_footprint_owner, sg::pipeline_stage_flag::compute,
                           _compute_arrays);
    flush_barriers();
    vkCmdDispatchIndirect(_buffer, buffer->_buffer, VkDeviceSize(offset_in_bytes));
}
} // namespace sg::backend::vulkan
