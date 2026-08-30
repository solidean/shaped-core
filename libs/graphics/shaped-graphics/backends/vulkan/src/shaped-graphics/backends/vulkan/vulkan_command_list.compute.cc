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
    _bound_groups.clear_resize_to_filled(_bound_pipeline_layout->_groups.size(), nullptr);
}

void vulkan_command_list::compute_bind_group(int group_index, sg::binding_group const& group)
{
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
    CC_ASSERT(vg->layout == _bound_pipeline_layout->_groups[group_index], "binding_group's layout does not match the "
                                                                          "pipeline layout's slot");
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

void vulkan_command_list::compute_declare_array_buffer_access(cc::string_view binding_name,
                                                              cc::span<sg::array_buffer_access const> elements)
{
    CC_ASSERT(!binding_name.empty(), "declare_array_buffer_access requires a binding name");

    // Arrays / bindless are not auto-tracked: which elements a shader indexes, and how, cannot be inferred, so the
    // caller declares it here.
    // Held until the next dispatch resolves it against the bound groups.
    auto declare = vulkan_array_buffer_declare{.name = cc::string(binding_name), .elements = {}};
    declare.elements.push_back_range(elements);
    _pending_array_buffer_declares.push_back(cc::move(declare));
}

void vulkan_command_list::compute_declare_array_texture_access(cc::string_view binding_name,
                                                               cc::span<sg::array_texture_access const> elements)
{
    CC_ASSERT(!binding_name.empty(), "declare_array_texture_access requires a binding name");
    auto declare = vulkan_array_texture_declare{.name = cc::string(binding_name), .elements = {}};
    declare.elements.push_back_range(elements);
    _pending_array_texture_declares.push_back(cc::move(declare));
}

void vulkan_command_list::declare_array_accesses()
{
    // The bound groups' array bindings, resolved by name — also the accounting set: every array binding must be
    // covered by a declaration, since which elements a shader indexes cannot be inferred and silently skipping one
    // would leave its resources untracked (wrong layouts, missed hazards).
    auto const find_array_binding = [&](cc::string_view name, bool want_texture) -> vulkan_array_binding const*
    {
        for (auto const* bound_group : _bound_groups)
        {
            if (bound_group == nullptr)
                continue;
            for (auto const& ab : bound_group->array_bindings)
                if (ab.name == name && ab.is_texture == want_texture)
                    return &ab;
        }
        return nullptr;
    };

    for (auto const& declare : _pending_array_buffer_declares)
    {
        auto const* ab = find_array_binding(declare.name, false);
        CC_ASSERT(ab != nullptr, "declare_array_buffer_access names no buffer array binding of a bound group");
        for (auto const& e : declare.elements)
        {
            CC_ASSERT(e.index >= 0 && e.index < int(ab->elements.size()), "declared array element index out of range");
            auto const& element = ab->elements[e.index];
            CC_ASSERT(!element.is_vacant(), "declared array element is vacant (nothing is bound there)");
            track_buffer_access(*element.buffer, e.stages, e.access);
        }
    }

    for (auto const& declare : _pending_array_texture_declares)
    {
        auto const* ab = find_array_binding(declare.name, true);
        CC_ASSERT(ab != nullptr, "declare_array_texture_access names no texture array binding of a bound group");
        for (auto const& e : declare.elements)
        {
            CC_ASSERT(e.index >= 0 && e.index < int(ab->elements.size()), "declared array element index out of range");
            auto const& element = ab->elements[e.index];
            CC_ASSERT(!element.is_vacant(), "declared array element is vacant (nothing is bound there)");
            track_texture_access(*element.texture, element.range, e.stages, e.access, e.layout);
        }
    }

#if CC_ASSERT_ENABLED
    // The reverse direction of the accounting: an undeclared array binding is a hard error, not "no access" — an
    // empty-span declaration is the way to say a dispatch touches no elements of an array.
    for (auto const* bound_group : _bound_groups)
    {
        if (bound_group == nullptr)
            continue;
        for (auto const& ab : bound_group->array_bindings)
        {
            bool declared = false;
            if (ab.is_texture)
            {
                for (auto const& declare : _pending_array_texture_declares)
                    declared |= declare.name == ab.name;
            }
            else
            {
                for (auto const& declare : _pending_array_buffer_declares)
                    declared |= declare.name == ab.name;
            }
            CC_ASSERT(declared, "a bound array binding has no declare_array_*_access for this dispatch (declare an "
                                "empty span if it is unused)");
        }
    }
#endif

    _pending_array_buffer_declares.clear();
    _pending_array_texture_declares.clear();
}

void vulkan_command_list::compute_dispatch(int x, int y, int z)
{
    CC_ASSERT(x >= 0 && y >= 0 && z >= 0, "dispatch group counts must be non-negative");
    CC_ASSERT(!_in_render_pass, "dispatch must not be recorded inside a rendering scope; close the scope first");

    // Declare each bound group's shader accesses before the dispatch: the tracker emits any hazard barrier (a prior
    // copy_write → shader_read RAW, a WAW between two dispatches), and — unlike dx12 — carries the access across
    // command lists, since Vulkan has no state decay to ride on.
    for (auto const* bound_group : _bound_groups)
    {
        if (bound_group == nullptr)
            continue;

        for (auto const& view : bound_group->hazard_views)
            if (view.buffer != nullptr)
                track_buffer_access(*view.buffer, sg::pipeline_stage_flag::compute, sg::shader_access_of(view.access));

        // Bound textures also transition to the layout their access class needs (a sampled texture to
        // shader_readonly, a storage texture to shader_readwrite) — the inferred layout is shader_layout_of.
        for (auto const& tv : bound_group->texture_hazard_views)
            track_texture_access(*tv.texture, tv.range, sg::pipeline_stage_flag::compute,
                                 sg::shader_access_of(tv.access), sg::shader_layout_of(tv.access));
    }

    // Array bindings are not auto-tracked — apply (and account for) the caller's explicit declarations.
    declare_array_accesses();

    // Emit every hazard the bound resources declared, batched, right before the dispatch consumes them.
    flush_barriers();
    vkCmdDispatch(_buffer, u32(x), u32(y), u32(z));
}
} // namespace sg::backend::vulkan
