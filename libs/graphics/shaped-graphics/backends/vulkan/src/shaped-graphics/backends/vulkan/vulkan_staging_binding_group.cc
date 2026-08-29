#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_sampler.hh>
#include <shaped-graphics/backends/vulkan/vulkan_staging_binding_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>
#include <shaped-graphics/backends/vulkan/vulkan_view_desc.hh>
#include <shaped-graphics/binding/binding.hh>

namespace sg::backend::vulkan
{
cc::result<vulkan_staging_binding_group_handle> vulkan_staging_binding_group::create(vulkan_context& ctx,
                                                                                     vulkan_binding_group_layout_handle layout)
{
    CC_ASSERT(layout != nullptr, "staging_binding_group requires a binding_group_layout");
    auto const bindings = layout->bindings();

    // The flattened numbering the base addresses descriptors in: every binding's elements contiguous, in declaration
    // order, so that adding an element index to a binding's first descriptor lands on that element.
    // A static sampler gets -1, which is how the base learns it cannot be set.
    auto descriptor_offsets = cc::vector<int>::create_uninitialized(bindings.size());
    cc::vector<isize> byte_offsets;
    for (isize i = 0; i < bindings.size(); ++i)
    {
        bool const is_static_sampler = layout->_slot_samplers[i] != VK_NULL_HANDLE;
        descriptor_offsets[i] = is_static_sampler ? -1 : int(byte_offsets.size());
        for (int e = 0; e < int(bindings[i].count); ++e)
            byte_offsets.push_back(layout->descriptor_offset_of(i, e));
    }

    auto group = std::make_shared<vulkan_staging_binding_group>(ctx, cc::move(layout), cc::move(descriptor_offsets),
                                                                cc::move(byte_offsets));
    auto r = group->initialize();
    CC_RETURN_IF_ERROR(r);
    return group;
}

vulkan_staging_binding_group::vulkan_staging_binding_group(vulkan_context& ctx,
                                                           vulkan_binding_group_layout_handle layout,
                                                           cc::vector<int> descriptor_offsets,
                                                           cc::vector<isize> byte_offsets)
  : sg::staging_binding_group(layout, cc::move(descriptor_offsets)),
    _ctx(ctx),
    _vk_layout(cc::move(layout)),
    _offsets(cc::move(byte_offsets))
{
    _image = cc::vector<byte>::create_filled(_vk_layout->_size_in_bytes, byte(0));
    _resources = cc::vector<staged>::create_defaulted(_offsets.size());
}

vulkan_staging_binding_group::~vulkan_staging_binding_group() = default;

cc::result<cc::unit> vulkan_staging_binding_group::initialize()
{
    auto const bindings = _vk_layout->bindings();

    // A zeroed image is already every view binding's empty value — that is what a null descriptor is here — so only
    // the sampler bindings need a write.
    // A static sampler's descriptor comes from the layout and is never touched again; a dynamic one starts at the
    // default sampler state, which is what unset_sampler means.
    for (isize i = 0; i < bindings.size(); ++i)
    {
        if (!sg::is_sampler(bindings[i].type))
            continue;

        VkSampler sampler = _vk_layout->_slot_samplers[i];
        if (sampler == VK_NULL_HANDLE)
        {
            sampler = _ctx._samplers.acquire(sg::sampler{});
            if (sampler == VK_NULL_HANDLE)
                return cc::error("staging_binding_group: could not create the default sampler");
        }
        // Qualified: the member overload below hides the free function this needs.
        sg::backend::vulkan::write_sampler_descriptor(_ctx, sampler,
                                                      _image.data() + _vk_layout->descriptor_offset_of(i, 0));
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
            res = {.buffer = {}, .texture = cc::move(texture), .range = tv->range, .access = tv->access};
        }
        else if (auto const* bv = sg::try_as_buffer_view(view); bv != nullptr)
        {
            auto buffer = std::dynamic_pointer_cast<vulkan_buffer const>(bv->buffer);
            CC_ASSERT(buffer != nullptr, "bound buffer is not a vulkan buffer");
            res = {.buffer = cc::move(buffer), .texture = {}, .range = {}, .access = bv->access};
        }
        // else: the null acceleration structure, which references nothing and tracks nothing.
    }
}

void vulkan_staging_binding_group::clear_view_descriptors(int first_descriptor, sg::binding const& b, int count)
{
    auto const size = size_t(descriptor_size_of(_ctx, b.type));
    for (int k = 0; k < count; ++k)
    {
        int const flat = first_descriptor + k;

        // The empty value of a view descriptor is a zeroed one — see write_view_descriptor's vacant arm, which this
        // deliberately spells the same way rather than calling through with a vacant_view.
        byte* const dst = at(flat);
        for (size_t i = 0; i < size; ++i)
            dst[i] = byte(0);
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
    // The resource references the staged descriptors point at, in the two shapes a group needs: a scalar binding is
    // auto-tracked through the hazard vectors, an array binding is declared per dispatch.
    cc::vector<vulkan_buffer_handle> referenced;
    cc::vector<vulkan_texture_handle> referenced_textures;
    cc::vector<vulkan_hazard_view> hazard_views;
    cc::vector<vulkan_texture_hazard_view> texture_hazard_views;
    cc::vector<vulkan_array_binding> array_bindings;

    auto const bindings = _vk_layout->bindings();
    int flat = 0;
    for (isize i = 0; i < bindings.size(); ++i)
    {
        auto const& b = bindings[i];
        if (sg::is_sampler(b.type))
        {
            flat += int(b.count);
            continue;
        }

        if (b.is_array())
        {
            auto ab = vulkan_array_binding{.name = b.name,
                                           .is_texture = sg::shape_of(b.type) == sg::view_shape::texture,
                                           .elements = {}};
            for (int e = 0; e < int(b.count); ++e)
            {
                auto const& res = _resources[flat + e];
                ab.elements.push_back({.buffer = res.buffer, .texture = res.texture, .range = res.range});
            }
            array_bindings.push_back(cc::move(ab));
        }
        else
        {
            auto const& res = _resources[flat];
            if (res.texture != nullptr)
            {
                referenced_textures.push_back(res.texture);
                texture_hazard_views.push_back({res.texture, res.range, res.access});
            }
            else if (res.buffer != nullptr)
            {
                referenced.push_back(res.buffer);
                hazard_views.push_back({res.buffer, res.access});
            }
            // else: a scalar slot with no resource — the null acceleration structure, which tracks nothing.
        }
        flat += int(b.count);
    }

    auto r = vulkan_binding_group::create_from_image(_ctx, _vk_layout, _image, cc::move(referenced),
                                                     cc::move(referenced_textures), cc::move(hazard_views),
                                                     cc::move(texture_hazard_views), cc::move(array_bindings));
    CC_RETURN_IF_ERROR(r);
    return sg::binding_group_handle(cc::move(r.value()));
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
cc::result<vulkan_staging_binding_group_handle> vulkan_context::create_vulkan_staging_binding_group(
    sg::binding_group_layout_handle const& layout,
    sg::lifetime_scope scope)
{
    CC_ASSERT(scope == sg::lifetime_scope::persistent, "a staging_binding_group is persistent — it exists to outlive "
                                                       "the epoch that built it");
    CC_ASSERT(layout != nullptr, "staging_binding_group requires a binding_group_layout");
    auto vk_layout = std::dynamic_pointer_cast<vulkan_binding_group_layout const>(layout);
    CC_ASSERT(vk_layout != nullptr, "binding_group_layout is not a vulkan one");
    return vulkan_staging_binding_group::create(*this, cc::move(vk_layout));
}
} // namespace sg::backend::vulkan
