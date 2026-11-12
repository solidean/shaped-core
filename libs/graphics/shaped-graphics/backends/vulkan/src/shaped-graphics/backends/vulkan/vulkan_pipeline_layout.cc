#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group_layout.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_pipeline_layout.hh>
#include <shaped-graphics/backends/vulkan/vulkan_sampler.hh>
#include <shaped-graphics/backends/vulkan/vulkan_view_desc.hh>
#include <shaped-graphics/binding/impl/layout_hash.hh>
#include <shaped-graphics/binding/reserved_bindings.hh>
#include <shaped-graphics/context/impl/debug_windows.hh>
#include <shaped-graphics/raytracing/impl/software_trace.hh>

namespace sg::backend::vulkan
{
cc::result<vulkan_pipeline_layout_handle> vulkan_pipeline_layout::create(vulkan_context& ctx,
                                                                         sg::pipeline_layout_description const& desc)
{
    // The cap is sg's rather than vulkan's: a caller gets max_binding_groups slots on every backend, and the one
    // above them is sg::reserved_binding_group.
    // A small_vector still heap-grows past its inline size, so this is a real check rather than a restatement.
    if (int(desc.groups.size()) > sg::max_binding_groups)
        return cc::error("pipeline_layout: more group slots than max_binding_groups");

    auto const hash = sg::impl::pipeline_layout_hash(desc);

    auto bound_samplers = cc::vector<VkSampler>();
    auto reserved_set_layout = VkDescriptorSetLayout(VK_NULL_HANDLE);
    auto empty_set_layout = VkDescriptorSetLayout(VK_NULL_HANDLE);
    auto const destroy_reserved = [&]
    {
        if (reserved_set_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(ctx._device, reserved_set_layout, nullptr);
        if (empty_set_layout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(ctx._device, empty_set_layout, nullptr);
    };

    // With diagnostics set 3 holds the debug window too, which an embedded-sampler set cannot
    auto const has_debug_window = sg::impl::debug_windows::k_enabled;
    auto traces_in_software = false;
    auto acceleration_bases = cc::vector<isize>();
    auto acceleration_members = isize(0);
    for (auto const& g : desc.groups)
    {
        acceleration_bases.push_back(acceleration_members);
        acceleration_members += sg::impl::acceleration_count(g->bindings());
        traces_in_software = traces_in_software || sg::impl::traces_in_software(g->bindings());
    }
    // the unbounded arrays' lengths, in the order of the groups that hold one, which sg writes from the bound groups
    auto length_bases = cc::vector<isize>();
    auto lengths = isize(0);
    for (auto const& g : desc.groups)
        length_bases.push_back(sg::impl::has_unbounded_array(g->bindings()) ? lengths++ : isize(-1));
    CC_ASSERT(lengths <= sg::impl::array_lengths_capacity, "a pipeline binds at most 16 unbounded binding arrays");
    auto const has_array_lengths = lengths > 0;
    // SGL's own SPIR-V traces every acceleration member by its address, which any package may compile to on any context
    auto const traces_by_address = acceleration_members > 0;
    CC_ASSERT(acceleration_members <= sg::impl::acceleration_address_capacity,
              "a vulkan pipeline binds at most 16 acceleration structures, whose addresses set 3 holds");
    auto const polyfill_slots = sg::impl::polyfill_slots_of(desc.mesh_polyfill);
    auto const has_mesh_slots = !polyfill_slots.empty();
    auto const has_regions = has_debug_window || traces_in_software || has_array_lengths || desc.dispatch_grid
                          || traces_by_address || has_mesh_slots;
    if (!desc.static_samplers.empty() || has_regions)
    {
        auto reserved_bindings = cc::vector<VkDescriptorSetLayoutBinding>();
        for (isize i = 0; i < desc.static_samplers.size(); ++i)
        {
            // sg refused a malformed or colliding one before the backend saw it.
            auto const& s = desc.static_samplers[i];

            // The context's cache owns the sampler, and outlives every in-flight use of it.
            auto const sampler = ctx._samplers.acquire(s.sampler);
            if (sampler == VK_NULL_HANDLE)
            {
                destroy_reserved();
                return cc::error(
                    cc::format("pipeline_layout: could not create the bound sampler for '{}'", s.binding.name));
            }
            bound_samplers.push_back(sampler);
        }
        // Filled only once every sampler exists, since a binding points into `bound_samplers`.
        for (isize i = 0; i < desc.static_samplers.size(); ++i)
            reserved_bindings.push_back(VkDescriptorSetLayoutBinding{
                .binding = desc.static_samplers[i].binding.index + 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_ALL,
                .pImmutableSamplers = has_regions ? nullptr : &bound_samplers[i],
            });
        if (has_debug_window)
            reserved_bindings.push_back(VkDescriptorSetLayoutBinding{
                .binding = u32(sg::reserved_debug_window.vulkan_binding),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_ALL,
            });
        // The software walk's pool and roots, at the bindings EMIT-135 declares them at.
        if (traces_in_software)
        {
            reserved_bindings.push_back(VkDescriptorSetLayoutBinding{
                .binding = u32(sg::reserved_acceleration_pool.vulkan_binding),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_ALL,
            });
            reserved_bindings.push_back(VkDescriptorSetLayoutBinding{
                .binding = u32(sg::reserved_acceleration_roots.vulkan_binding),
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_ALL,
            });
        }
        if (has_array_lengths)
            reserved_bindings.push_back(VkDescriptorSetLayoutBinding{
                .binding = u32(sg::reserved_array_lengths.vulkan_binding),
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_ALL,
            });
        if (desc.dispatch_grid)
            reserved_bindings.push_back(VkDescriptorSetLayoutBinding{
                .binding = u32(sg::reserved_dispatch_grid.vulkan_binding),
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_ALL,
            });
        if (traces_by_address)
            reserved_bindings.push_back(VkDescriptorSetLayoutBinding{
                .binding = u32(sg::reserved_acceleration_addresses.vulkan_binding),
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_ALL,
            });
        // a polyfill's slots and records, each written by one half and read by another
        for (auto const& slot : polyfill_slots)
            reserved_bindings.push_back(VkDescriptorSetLayoutBinding{
                .binding = u32(slot.address->vulkan_binding),
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_ALL,
            });

        auto const reserved_info = VkDescriptorSetLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags
            = has_regions
                ? VkDescriptorSetLayoutCreateFlags(VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT)
                : VkDescriptorSetLayoutCreateFlags(VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT
                                                   | VK_DESCRIPTOR_SET_LAYOUT_CREATE_EMBEDDED_IMMUTABLE_SAMPLERS_BIT_EXT),
            .bindingCount = u32(reserved_bindings.size()),
            .pBindings = reserved_bindings.data(),
        };
        if (VkResult const r = vkCreateDescriptorSetLayout(ctx._device, &reserved_info, nullptr, &reserved_set_layout);
            r != VK_SUCCESS)
        {
            destroy_reserved();
            return vulkan_error(r, "vkCreateDescriptorSetLayout (reserved set) failed");
        }

        auto const empty_info = VkDescriptorSetLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        };
        if (VkResult const r = vkCreateDescriptorSetLayout(ctx._device, &empty_info, nullptr, &empty_set_layout);
            r != VK_SUCCESS)
        {
            destroy_reserved();
            return vulkan_error(r, "vkCreateDescriptorSetLayout (empty set) failed");
        }
    }

    // A group's position in the description is its bind slot, and the same index is the `firstSet` a bind command
    // passes — so the ordering here is the whole of what dx12 needs a root-parameter index table for.
    cc::vector<VkDescriptorSetLayout> set_layouts;
    cc::vector<sg::binding_group_layout_handle> groups;
    for (auto const& group : desc.groups)
    {
        CC_ASSERT(group != nullptr, "a pipeline layout's group is null");
        auto const vk_group = std::dynamic_pointer_cast<vulkan_binding_group_layout const>(group);
        CC_ASSERT(vk_group != nullptr, "binding group layout is not a vulkan one");
        set_layouts.push_back(vk_group->_layout);
        groups.push_back(group);
    }
    if (reserved_set_layout != VK_NULL_HANDLE)
    {
        while (set_layouts.size() < sg::reserved_binding_group)
            set_layouts.push_back(empty_set_layout);
        set_layouts.push_back(reserved_set_layout);
    }

    // Inline constants become one push-constant range visible to every stage, matching how the binding itself is
    // declared: sg carries no stage mask on it.
    int inline_bytes = 0;
    VkPushConstantRange push_range = {};
    if (desc.inline_constants.has_value())
    {
        auto const& b = desc.inline_constants.value();
        CC_ASSERT(b.type == sg::binding_type::constants_buffer, "inline constants must be a constants_buffer binding");
        CC_ASSERT(b.block_size.has_value() && b.block_size.value() > 0 && b.block_size.value() % 4 == 0,
                  "inline constants need a block_size that is a positive multiple of 4");
        inline_bytes = int(b.block_size.value());
        push_range = VkPushConstantRange{
            .stageFlags = VK_SHADER_STAGE_ALL,
            .offset = 0,
            .size = u32(inline_bytes),
        };
    }

    auto const info = VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = u32(set_layouts.size()),
        .pSetLayouts = set_layouts.data(),
        .pushConstantRangeCount = inline_bytes > 0 ? 1u : 0u,
        .pPushConstantRanges = inline_bytes > 0 ? &push_range : nullptr,
    };

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (VkResult const r = vkCreatePipelineLayout(ctx._device, &info, nullptr, &layout); r != VK_SUCCESS)
    {
        destroy_reserved();
        return vulkan_error(r, "vkCreatePipelineLayout failed");
    }

    auto result = std::make_shared<vulkan_pipeline_layout>(ctx, hash, layout, cc::move(groups), desc.inline_constants,
                                                           inline_bytes, desc.dispatch_grid, desc.static_samplers,
                                                           desc.mesh_polyfill);
    result->_reserved_set_layout = reserved_set_layout;
    result->_empty_set_layout = empty_set_layout;
    result->_acceleration_bases = cc::move(acceleration_bases);
    result->_traces_in_software = traces_in_software;
    result->_traces_by_address = traces_by_address;
    result->_has_array_lengths = has_array_lengths;
    result->_length_bases = cc::move(length_bases);
    if (has_regions && reserved_set_layout != VK_NULL_HANDLE)
    {
        auto size = VkDeviceSize(0);
        ctx._descriptor_functions.get_layout_size(ctx._device, reserved_set_layout, &size);
        result->_reserved_has_regions = true;
        result->_reserved_size = isize(size);
        for (isize i = 0; i < desc.static_samplers.size(); ++i)
        {
            auto offset = VkDeviceSize(0);
            ctx._descriptor_functions.get_binding_offset(ctx._device, reserved_set_layout,
                                                         desc.static_samplers[i].binding.index + 1, &offset);
            result->_reserved_samplers.push_back(bound_samplers[i]);
            result->_reserved_sampler_offsets.push_back(isize(offset));
        }
        auto offset = VkDeviceSize(0);
        if (has_debug_window)
        {
            ctx._descriptor_functions.get_binding_offset(ctx._device, reserved_set_layout, 19, &offset);
            result->_debug_window_offset = isize(offset);
        }
        if (traces_in_software)
        {
            ctx._descriptor_functions.get_binding_offset(ctx._device, reserved_set_layout, 17, &offset);
            result->_pool_offset = isize(offset);
            ctx._descriptor_functions.get_binding_offset(ctx._device, reserved_set_layout, 18, &offset);
            result->_roots_offset = isize(offset);
        }
        if (has_array_lengths)
        {
            ctx._descriptor_functions.get_binding_offset(ctx._device, reserved_set_layout,
                                                         u32(sg::reserved_array_lengths.vulkan_binding), &offset);
            result->_lengths_offset = isize(offset);
        }
        if (desc.dispatch_grid)
        {
            ctx._descriptor_functions.get_binding_offset(ctx._device, reserved_set_layout,
                                                         u32(sg::reserved_dispatch_grid.vulkan_binding), &offset);
            result->_grid_offset = isize(offset);
        }
        if (traces_by_address)
        {
            ctx._descriptor_functions.get_binding_offset(
                ctx._device, reserved_set_layout, u32(sg::reserved_acceleration_addresses.vulkan_binding), &offset);
            result->_addresses_offset = isize(offset);
        }
        for (auto const& slot : polyfill_slots)
        {
            ctx._descriptor_functions.get_binding_offset(ctx._device, reserved_set_layout,
                                                         u32(slot.address->vulkan_binding), &offset);
            (slot.is_task ? result->_task_slots_offset : result->_mesh_slots_offset) = isize(offset);
            (slot.is_task ? result->_holds_task_slots : result->_holds_mesh_slots) = true;
        }
    }
    return vulkan_pipeline_layout_handle(cc::move(result));
}

void vulkan_pipeline_layout::bind_reserved(VkCommandBuffer buffer,
                                           VkPipelineBindPoint bind_point,
                                           sg::raw_buffer_handle const& debug_window) const
{
    // a set 3 holding the op's roots or lengths is the op's to bind (bind_software_trace)
    if (_reserved_set_layout == VK_NULL_HANDLE || binds_set_per_op())
        return;
    if (!_reserved_has_regions)
    {
        _ctx._descriptor_functions.cmd_bind_embedded_samplers(buffer, bind_point, _layout,
                                                              u32(sg::reserved_binding_group));
        return;
    }
    CC_ASSERT(debug_window != nullptr, "set 3 holds the debug window, so a bind needs one");

    // the region of this window, written the first time the layout meets it
    auto const offset = _reserved_regions.lock(
        [&](cc::vector<reserved_region>& regions)
        {
            for (auto const& r : regions)
                if (r.window == debug_window.get())
                    return r.range.offset;
            auto range = _ctx._descriptor_heap.allocate_persistent(_reserved_size);
            CC_ASSERT(range.size > 0, "the descriptor heap has no room for set 3");
            auto* const base = _ctx._descriptor_heap.mapped_at(range);
            for (isize i = 0; i < _reserved_samplers.size(); ++i)
                write_sampler_descriptor(_ctx, _reserved_samplers[i], base + _reserved_sampler_offsets[i]);

            auto const& window = static_cast<vulkan_buffer const&>(*debug_window);
            write_storage_descriptor(window._device_address, debug_window->size_in_bytes(), base + _debug_window_offset);
            auto const at = range.offset;
            regions.push_back({.window = debug_window.get(), .range = cc::move(range)});
            return at;
        });

    u32 const buffer_index = 0;
    auto const vk_offset = VkDeviceSize(offset);
    _ctx._descriptor_functions.cmd_set_descriptor_offsets(buffer, bind_point, _layout, u32(sg::reserved_binding_group),
                                                          1, &buffer_index, &vk_offset);
}

void vulkan_pipeline_layout::write_storage_descriptor(VkDeviceAddress address, isize size_in_bytes, byte* at) const
{
    auto range = VkDescriptorAddressInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
        .address = address,
        .range = VkDeviceSize(size_in_bytes),
        .format = VK_FORMAT_UNDEFINED,
    };
    auto const info = VkDescriptorGetInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .data = {.pStorageBuffer = &range},
    };
    _ctx._descriptor_functions.get_descriptor(_ctx._device, &info,
                                              size_t(descriptor_size_of(_ctx, sg::binding_type::buffer)), at);
}

void vulkan_pipeline_layout::bind_software_trace(VkCommandBuffer buffer,
                                                 VkPipelineBindPoint bind_point,
                                                 sg::raw_buffer_handle const& debug_window,
                                                 sg::impl::software_pool_storage const& pool,
                                                 cc::span<u32 const> roots,
                                                 cc::span<u32 const> lengths,
                                                 cc::span<u32 const> grid,
                                                 cc::span<u32 const> addresses,
                                                 VkDeviceAddress mesh_slots,
                                                 isize mesh_slots_bytes,
                                                 VkDeviceAddress task_slots,
                                                 isize task_slots_bytes) const
{
    // The set and the values it names, in one transient range of the heap, each a uniform buffer behind the set.
    // 256 is the largest minUniformBufferOffsetAlignment Vulkan allows.
    constexpr auto uniform_alignment = isize(256);
    auto const roots_bytes = _traces_in_software ? roots.size() * isize(sizeof(u32)) : isize(0);
    auto const lengths_bytes = _has_array_lengths ? lengths.size() * isize(sizeof(u32)) : isize(0);
    auto const grid_bytes = has_dispatch_grid() ? grid.size() * isize(sizeof(u32)) : isize(0);
    auto const addresses_bytes = _traces_by_address ? addresses.size() * isize(sizeof(u32)) : isize(0);
    auto range = _ctx._descriptor_heap.allocate_transient(_reserved_size + 4 * uniform_alignment + roots_bytes
                                                          + lengths_bytes + grid_bytes + addresses_bytes);
    CC_ASSERT(range.size > 0, "the descriptor heap has no room for set 3");
    auto* const base = _ctx._descriptor_heap.mapped_at(range);
    for (isize i = 0; i < _reserved_samplers.size(); ++i)
        write_sampler_descriptor(_ctx, _reserved_samplers[i], base + _reserved_sampler_offsets[i]);
    if (debug_window != nullptr)
        write_storage_descriptor(static_cast<vulkan_buffer const&>(*debug_window)._device_address,
                                 debug_window->size_in_bytes(), base + _debug_window_offset);

    // `values` copied behind the set at `at`, and the uniform buffer naming them written at `descriptor`
    auto const write_uniform = [&](cc::span<u32 const> values, isize at, isize descriptor)
    {
        auto const bytes = values.size() * isize(sizeof(u32));
        cc::memcpy(base + (at - range.offset), values.data(), size_t(bytes));
        auto uniform = VkDescriptorAddressInfoEXT{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
            .address = _ctx._descriptor_heap.device_address() + VkDeviceAddress(at),
            .range = VkDeviceSize(bytes),
            .format = VK_FORMAT_UNDEFINED,
        };
        auto const info = VkDescriptorGetInfoEXT{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .data = {.pUniformBuffer = &uniform},
        };
        _ctx._descriptor_functions.get_descriptor(
            _ctx._device, &info, size_t(descriptor_size_of(_ctx, sg::binding_type::constants_buffer)), base + descriptor);
    };
    auto next = cc::align_up(range.offset + _reserved_size, uniform_alignment);
    if (_traces_in_software)
    {
        write_storage_descriptor(pool.gpu_address, pool.size_in_bytes, base + _pool_offset);
        write_uniform(roots, next, _roots_offset);
        next = cc::align_up(next + roots_bytes, uniform_alignment);
    }
    if (_has_array_lengths)
    {
        write_uniform(lengths, next, _lengths_offset);
        next = cc::align_up(next + lengths_bytes, uniform_alignment);
    }
    if (has_dispatch_grid())
    {
        write_uniform(grid, next, _grid_offset);
        next = cc::align_up(next + grid_bytes, uniform_alignment);
    }
    if (_traces_by_address)
        write_uniform(addresses, next, _addresses_offset);
    if (_holds_mesh_slots)
    {
        CC_ASSERT(mesh_slots != 0, "a polyfilled mesh stage's set 3 names the draw's slot buffer");
        write_storage_descriptor(mesh_slots, mesh_slots_bytes, base + _mesh_slots_offset);
    }
    if (_holds_task_slots)
    {
        CC_ASSERT(task_slots != 0, "a polyfilled task stage's set 3 names the draw's record buffer");
        write_storage_descriptor(task_slots, task_slots_bytes, base + _task_slots_offset);
    }

    u32 const buffer_index = 0;
    auto const vk_offset = VkDeviceSize(range.offset);
    _ctx._descriptor_functions.cmd_set_descriptor_offsets(buffer, bind_point, _layout, u32(sg::reserved_binding_group),
                                                          1, &buffer_index, &vk_offset);
}

// Immediate rather than epoch-deferred: the layout objects are consumed at pipeline-creation and descriptor-allocation time, so no in-flight work names them.
void vulkan_pipeline_layout::release_backend_objects()
{
    // a region a list in flight may still read goes back to the heap once its epoch retires
    _reserved_regions.lock(
        [&](cc::vector<reserved_region>& regions)
        {
            for (auto& r : regions)
                if (r.range.size > 0)
                    _ctx.defer_until_retired([ctx = &_ctx, range = cc::move(r.range)]() mutable
                                             { ctx->_descriptor_heap.free_persistent(cc::move(range)); });
            regions.clear();
        });
    if (_layout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(_ctx._device, _layout, nullptr);
    if (_reserved_set_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(_ctx._device, _reserved_set_layout, nullptr);
    if (_empty_set_layout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(_ctx._device, _empty_set_layout, nullptr);
    _layout = VK_NULL_HANDLE;
    _reserved_set_layout = VK_NULL_HANDLE;
    _empty_set_layout = VK_NULL_HANDLE;
}

vulkan_pipeline_layout::~vulkan_pipeline_layout()
{
    this->release_backend_objects();
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
cc::result<vulkan_pipeline_layout_handle> vulkan_context::create_vulkan_pipeline_layout(
    sg::pipeline_layout_description const& desc)
{
    return vulkan_pipeline_layout::create(*this, desc);
}
} // namespace sg::backend::vulkan
