#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_acceleration_structure.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_sampler.hh>
#include <shaped-graphics/backends/vulkan/vulkan_staging_binding_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>
#include <shaped-graphics/backends/vulkan/vulkan_view_desc.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/raytracing/impl/software_trace.hh>

namespace sg::backend::vulkan
{
cc::result<vulkan_staging_binding_group_handle> vulkan_staging_binding_group::create(vulkan_context& ctx,
                                                                                     vulkan_binding_group_layout_handle layout,
                                                                                     int unbounded_capacity)
{
    CC_ASSERT(layout != nullptr, "staging_binding_group requires a binding_group_layout");
    auto const bindings = layout->bindings();
    // a snapshot carries descriptors, and the software walk's roots are no descriptor
    if (sg::impl::traces_in_software(bindings))
        return cc::error("staging_binding_group: a binding traced in software takes a binding_group");
    if (unbounded_capacity > int(layout->_unbounded_capacity))
        return cc::error(cc::format("staging_binding_group: an unbounded capacity of {} elements, past the device's {}",
                                    unbounded_capacity, layout->_unbounded_capacity));

    // The flattened numbering the base addresses descriptors in: every binding's elements contiguous, in declaration
    // order, so that adding an element index to a binding's first descriptor lands on that element.
    // A static sampler gets -1, which is how the base learns it cannot be set.
    auto descriptor_offsets = cc::vector<int>::create_uninitialized(bindings.size());
    cc::vector<isize> byte_offsets;
    for (isize i = 0; i < bindings.size(); ++i)
    {
        bool const is_static_sampler = layout->_slot_samplers[i] != VK_NULL_HANDLE;
        descriptor_offsets[i] = is_static_sampler ? -1 : int(byte_offsets.size());
        auto const count = bindings[i].is_unbounded() ? unbounded_capacity : int(bindings[i].count);
        for (int e = 0; e < count; ++e)
            byte_offsets.push_back(layout->descriptor_offset_of(i, e));
    }

    auto group = std::make_shared<vulkan_staging_binding_group>(ctx, cc::move(layout), cc::move(descriptor_offsets),
                                                                cc::move(byte_offsets), unbounded_capacity);
    auto r = group->initialize();
    CC_RETURN_IF_ERROR(r);
    return group;
}

vulkan_staging_binding_group::vulkan_staging_binding_group(vulkan_context& ctx,
                                                           vulkan_binding_group_layout_handle layout,
                                                           cc::vector<int> descriptor_offsets,
                                                           cc::vector<isize> byte_offsets,
                                                           int unbounded_capacity)
  : sg::staging_binding_group(layout, cc::move(descriptor_offsets), unbounded_capacity),
    _ctx(ctx),
    _vk_layout(cc::move(layout)),
    _offsets(cc::move(byte_offsets))
{
    _image = cc::vector<byte>::create_filled(_vk_layout->group_size_in_bytes(unbounded_capacity), byte(0));
    _resources = cc::vector<staged>::create_defaulted(_offsets.size());
}

vulkan_staging_binding_group::~vulkan_staging_binding_group() = default;

cc::result<cc::unit> vulkan_staging_binding_group::initialize()
{
    auto const bindings = _vk_layout->bindings();

    // Every view descriptor starts as the driver's null descriptor, which is not zeros on every device.
    // A static sampler's descriptor comes from the layout and is never touched again; a dynamic one starts at the
    // default sampler state, which is what unset_sampler means.
    for (isize i = 0; i < bindings.size(); ++i)
    {
        if (!sg::is_sampler(bindings[i].type))
        {
            for (auto element = 0; element < element_count(bindings[i]); ++element)
                write_null_descriptor(_ctx, bindings[i].type,
                                      _image.data() + _vk_layout->descriptor_offset_of(i, element));
            continue;
        }

        VkSampler sampler = _vk_layout->_slot_samplers[i];
        if (sampler == VK_NULL_HANDLE)
        {
            sampler = _ctx._samplers.acquire(sg::sampler{});
            if (sampler == VK_NULL_HANDLE)
                return cc::error("staging_binding_group: could not create the default sampler");
        }
        // Qualified: the member overload below hides the free function this needs.
        for (auto element = 0; element < int(bindings[i].count); ++element)
            sg::backend::vulkan::write_sampler_descriptor(_ctx, sampler,
                                                          _image.data() + _vk_layout->descriptor_offset_of(i, element));
    }
    return cc::unit{};
}

void vulkan_staging_binding_group::write_view_descriptors(int first_descriptor,
                                                          sg::binding const& b,
                                                          cc::span<sg::raw_view const> views)
{
    for (isize k = 0; k < views.size(); ++k)
    {
        auto const& view = views[k];
        int const flat = first_descriptor + int(k);
        write_view_descriptor(_ctx, _ctx._image_views, b, view, at(flat));

        // Replace what this descriptor referenced, releasing the previous resource.
        auto& res = _resources[flat];
        res = {};
        if (auto const* tv = sg::try_as_texture_view(view); tv != nullptr)
        {
            auto texture = std::dynamic_pointer_cast<vulkan_texture const>(tv->texture);
            CC_ASSERT(texture != nullptr, "bound texture is not a vulkan texture");
            res = {.buffer = {}, .texture = cc::move(texture), .range = tv->range, .bound_as = tv->bound_as};
        }
        else if (auto const* bv = sg::try_as_buffer_view(view); bv != nullptr)
        {
            auto buffer = std::dynamic_pointer_cast<vulkan_buffer const>(bv->buffer);
            CC_ASSERT(buffer != nullptr, "bound buffer is not a vulkan buffer");
            res = {.buffer = cc::move(buffer), .texture = {}, .range = {}, .bound_as = bv->bound_as};
        }
        else if (auto const* av = sg::try_as_tlas_view(view); av != nullptr && av->tlas != nullptr)
        {
            // A trace reads the TLAS's storage buffer, so that is what a minted group keeps alive and declares.
            // A null handle is the null acceleration structure rather than a mistake, and references nothing.
            auto const target = trace_target_of(*av->tlas);
            res = {.buffer = target.storage,
                   .texture = {},
                   .range = {},
                   .bound_as = sg::view_class::acceleration_structure,
                   .address = target.address};
        }
    }
}

void vulkan_staging_binding_group::clear_view_descriptors(int first_descriptor, sg::binding const& b, int count)
{
    for (int k = 0; k < count; ++k)
    {
        int const flat = first_descriptor + k;
        write_null_descriptor(_ctx, b.type, at(flat));
        _resources[flat] = {};
    }
}

void vulkan_staging_binding_group::write_sampler_descriptor(int descriptor_index, sg::sampler const& smp)
{
    VkSampler const sampler = _ctx._samplers.acquire(smp);

    // The setter this comes from returns void, so a sampler that cannot be created can only be reported here.
    // It means the device is out of memory, which the next allocation reports as a failure the caller can act on.
    if (sampler == VK_NULL_HANDLE)
    {
        CC_LOG_ERROR("staging_binding_group: could not create a sampler; the binding keeps its previous state.");
        return;
    }
    sg::backend::vulkan::write_sampler_descriptor(_ctx, sampler, at(descriptor_index));
}

cc::result<sg::binding_group_handle> vulkan_staging_binding_group::mint()
{
    _ctx.stat_totals().add(sg::stat::binding_groups_created);
    // The resource references the staged descriptors point at, in the two shapes a group needs: a scalar binding is
    // auto-tracked through the hazard vectors, an array binding is declared per dispatch.
    cc::vector<vulkan_buffer_handle> referenced;
    cc::vector<vulkan_texture_handle> referenced_textures;
    cc::vector<vulkan_hazard_view> hazard_views;
    cc::vector<vulkan_texture_hazard_view> texture_hazard_views;
    cc::vector<vulkan_array_binding> array_bindings;
    cc::vector<u64> acceleration_addresses;

    auto const bindings = _vk_layout->bindings();
    int flat = 0;
    // a staging group's unbounded array is as long as its capacity, a vacant element a null descriptor
    auto unbounded_length = u32(0);
    for (isize i = 0; i < bindings.size(); ++i)
    {
        auto const& b = bindings[i];
        if (b.is_unbounded())
            unbounded_length = u32(element_count(b));
        if (sg::is_sampler(b.type))
        {
            flat += element_count(b);
            continue;
        }

        if (b.is_array())
        {
            auto ab = vulkan_array_binding{.name = b.name,
                                           .is_texture = sg::shape_of(b.type) == sg::view_shape::texture,
                                           .binding = i,
                                           .bound_as = sg::view_class_of(b),
                                           .elements = {}};
            for (int e = 0; e < element_count(b); ++e)
            {
                auto const& res = _resources[flat + e];
                ab.elements.push_back({.buffer = res.buffer, .texture = res.texture, .range = res.range});
            }
            array_bindings.push_back(cc::move(ab));
        }
        else
        {
            auto const& res = _resources[flat];
            if (b.type == sg::binding_type::acceleration_structure)
            {
                auto const position = sg::impl::acceleration_position(bindings, b.index);
                while (acceleration_addresses.size() <= position)
                    acceleration_addresses.push_back(0);
                acceleration_addresses[position] = res.address;
            }
            if (res.texture != nullptr)
            {
                referenced_textures.push_back(res.texture);
                texture_hazard_views.push_back({res.texture, res.range, res.bound_as, i});
            }
            else if (res.buffer != nullptr)
            {
                referenced.push_back(res.buffer);
                hazard_views.push_back({res.buffer, res.bound_as, i});
            }
            // else: a scalar slot with no resource — the null acceleration structure, which tracks nothing.
        }
        flat += element_count(b);
    }

    auto r = vulkan_binding_group::create_from_image(
        _ctx, _vk_layout, _image, cc::move(referenced), cc::move(referenced_textures), cc::move(hazard_views),
        cc::move(texture_hazard_views), cc::move(array_bindings), unbounded_length, cc::move(acceleration_addresses));
    CC_RETURN_IF_ERROR(r);
    return sg::binding_group_handle(cc::move(r.value()));
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
cc::result<vulkan_staging_binding_group_handle> vulkan_context::create_vulkan_staging_binding_group(
    sg::binding_group_layout_handle const& layout,
    int unbounded_capacity,
    sg::lifetime_scope scope)
{
    CC_ASSERT(scope == sg::lifetime_scope::persistent, "a staging_binding_group is persistent — it exists to outlive "
                                                       "the epoch that built it");
    CC_ASSERT(layout != nullptr, "staging_binding_group requires a binding_group_layout");
    auto vk_layout = std::dynamic_pointer_cast<vulkan_binding_group_layout const>(layout);
    CC_ASSERT(vk_layout != nullptr, "binding_group_layout is not a vulkan one");
    return vulkan_staging_binding_group::create(*this, cc::move(vk_layout), unbounded_capacity);
}
} // namespace sg::backend::vulkan
