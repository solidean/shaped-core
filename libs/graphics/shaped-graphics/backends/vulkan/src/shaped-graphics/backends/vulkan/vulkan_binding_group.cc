#include <clean-core/common/assert.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_sampler.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>
#include <shaped-graphics/backends/vulkan/vulkan_view_desc.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/resource/views.hh>

namespace sg::backend::vulkan
{
namespace
{
/// The heap range one group of `layout` needs, or an error when the heap cannot supply it.
/// An empty layout takes no space, which is legal and yields an empty range.
cc::result<vulkan_descriptor_range> allocate_range(vulkan_context& ctx,
                                                   vulkan_binding_group_layout const& layout,
                                                   bool transient)
{
    if (layout._size_in_bytes == 0)
        return vulkan_descriptor_range{};

    auto range = transient ? ctx._descriptor_heap.allocate_transient(layout._size_in_bytes)
                           : ctx._descriptor_heap.allocate_persistent(layout._size_in_bytes);

    // Exhaustion is a recoverable runtime failure rather than a contract violation — a large bindless table is
    // exactly the case that hits it — so it is reported instead of asserted.
    if (range.is_empty())
        return cc::error("binding_group: descriptor heap exhausted (no free span fits the group)");
    return range;
}
} // namespace

vulkan_binding_group::~vulkan_binding_group()
{
    // A persistent group returns its range to the heap's free list, deferred until its last-using epoch retires.
    // A transient group's bytes are reclaimed by the ring on the epoch cycle, so there is nothing to free.
    if (_ctx == nullptr || transient || range.is_empty())
        return;

    vulkan_expiring_resource expiring;
    auto* const heap = &_ctx->_descriptor_heap;
    expiring.finalizers.push_back([heap, r = range]() mutable { heap->free_persistent(r); });
    _ctx->schedule_deferred_deletion(cc::move(expiring));
}

cc::result<vulkan_binding_group_handle> vulkan_binding_group::create(vulkan_context& ctx,
                                                                     vulkan_binding_group_layout_handle const& layout,
                                                                     cc::span<sg::named_view const> views,
                                                                     cc::span<sg::named_sampler const> samplers,
                                                                     sg::lifetime_scope scope)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_group_layout");

    auto group = std::make_shared<vulkan_binding_group>();
    group->_ctx = &ctx;
    group->layout = layout;
    group->transient = scope == sg::lifetime_scope::transient;
    group->creation_epoch = ctx.current_epoch();

    // Stored on the group before anything can fail, so the destructor frees a persistent range even when a later
    // validation error abandons creation partway.
    {
        auto r = allocate_range(ctx, *layout, group->transient);
        CC_RETURN_IF_ERROR(r);
        group->range = cc::move(r.value());
    }
    byte* const base = layout->_size_in_bytes == 0 ? nullptr : ctx._descriptor_heap.mapped_at(group->range);

    auto const bindings = layout->bindings();

    // A static sampler's descriptor is written from the layout's own VkSampler rather than baked into the set layout
    // as an immutable one, so it is the group that places it — see vulkan_binding_group_layout.
    for (isize i = 0; i < bindings.size(); ++i)
        if (layout->_slot_samplers[i] != VK_NULL_HANDLE)
            write_sampler_descriptor(ctx, layout->_slot_samplers[i], base + layout->descriptor_offset_of(i, 0));

    // Match each provided view list to a binding by name, validate it, and write its descriptors.
    // A scalar binding is auto-tracked: its view lands in the hazard vectors, declared at dispatch.
    // An array binding is not: its elements land in `array_bindings`, and the dispatching caller declares the used
    // ones via declare_array_*_access.
    auto view_filled = cc::vector<char>::create_filled(bindings.size(), char(0));
    for (auto const& nv : views)
    {
        isize slot = -1;
        for (isize i = 0; i < bindings.size(); ++i)
            if (!sg::is_sampler(bindings[i].type) && bindings[i].name == nv.name)
            {
                slot = i;
                break;
            }
        if (slot < 0)
            return cc::error(cc::format("binding_group: no view binding named '{}' in the layout", nv.name));

        auto const& b = bindings[slot];
        bool const is_array = b.is_array();
        auto const element_views = nv.view.span();
        if (element_views.size() != isize(b.count))
            return cc::error(cc::format("binding_group: '{}' takes {} view(s), {} provided (an array binding takes "
                                        "exactly one per element; a vacant element is sg::vacant_view)",
                                        nv.name, b.count, element_views.size()));
        CC_ASSERT(view_filled[slot] == char(0), "binding_group: a binding was provided more than once");
        view_filled[slot] = char(1);

        auto array_binding = vulkan_array_binding{.name = nv.name,
                                                  .is_texture = sg::shape_of(b.type) == sg::view_shape::texture,
                                                  .elements = {}};

        for (isize element = 0; element < element_views.size(); ++element)
        {
            auto const& view = element_views[element];
            if (!sg::accepts(b.type, view))
                return cc::error(
                    cc::format("binding_group: element {} of '{}' does not match its declared kind", element, nv.name));

            if (sg::is_vacant(view) && !is_array)
                return cc::error(cc::format("binding_group: '{}' — a vacant element is only valid in an array "
                                            "binding; a scalar binding must bind a resource",
                                            nv.name));
            if (auto const* tv = sg::try_as_texture_view(view); tv != nullptr && tv->texture == nullptr)
                return cc::error(cc::format("binding_group: '{}' — a texture view must bind a texture (a vacant "
                                            "array element is sg::vacant_view)",
                                            nv.name));
            if (auto const* bv = sg::try_as_buffer_view(view); bv != nullptr && bv->buffer == nullptr)
                return cc::error(cc::format("binding_group: '{}' — a buffer view must bind a buffer (a vacant array "
                                            "element is sg::vacant_view)",
                                            nv.name));
            if (sg::try_as_tlas_view(view) != nullptr && is_array)
                return cc::error(
                    cc::format("binding_group: '{}' — acceleration-structure arrays are not supported yet", nv.name));

            write_view_descriptor(ctx, ctx._image_views, b, view,
                                  base + layout->descriptor_offset_of(slot, int(element)));

            // What the group has to keep alive, and what a dispatch has to declare a hazard on.
            // A vacant element and a null acceleration structure both reference nothing, which is why neither
            // appears here.
            if (auto const* tv = sg::try_as_texture_view(view); tv != nullptr)
            {
                auto texture = std::dynamic_pointer_cast<vulkan_texture const>(tv->texture);
                CC_ASSERT(texture != nullptr, "bound texture is not a vulkan texture");
                if (is_array)
                    array_binding.elements.push_back({.buffer = {}, .texture = cc::move(texture), .range = tv->range});
                else
                {
                    group->referenced_textures.push_back(cc::move(texture));
                    group->texture_hazard_views.push_back({group->referenced_textures.back(), tv->range, tv->access});
                }
            }
            else if (auto const* bv = sg::try_as_buffer_view(view); bv != nullptr)
            {
                auto buffer = std::dynamic_pointer_cast<vulkan_buffer const>(bv->buffer);
                CC_ASSERT(buffer != nullptr, "bound buffer is not a vulkan buffer");
                if (is_array)
                    array_binding.elements.push_back({.buffer = cc::move(buffer), .texture = {}, .range = {}});
                else
                {
                    group->referenced.push_back(cc::move(buffer));
                    group->hazard_views.push_back({group->referenced.back(), bv->access});
                }
            }
            else if (is_array)
                array_binding.elements.push_back({}); // vacant
        }

        if (is_array)
            group->array_bindings.push_back(cc::move(array_binding));
    }

    // Match each provided dynamic sampler by name and write its descriptor.
    // A static sampler was already written above, so supplying one is an error rather than an override.
    auto sampler_filled = cc::vector<char>::create_filled(bindings.size(), char(0));
    for (auto const& ns : samplers)
    {
        isize slot = -1;
        for (isize i = 0; i < bindings.size(); ++i)
            if (sg::is_sampler(bindings[i].type) && bindings[i].name == ns.name)
            {
                slot = i;
                break;
            }
        if (slot < 0)
            return cc::error(cc::format("binding_group: no sampler binding named '{}' in the layout", ns.name));
        if (layout->_slot_samplers[slot] != VK_NULL_HANDLE)
            return cc::error(cc::format("binding_group: sampler '{}' is static — it is fixed by the layout and must "
                                        "not be supplied per group",
                                        ns.name));
        CC_ASSERT(sampler_filled[slot] == char(0), "binding_group: a sampler was provided more than once");
        CC_ASSERT(bindings[slot].count == 1, "dynamic sampler arrays are not supported yet");
        sampler_filled[slot] = char(1);

        VkSampler const sampler = ctx._samplers.acquire(ns.sampler);
        if (sampler == VK_NULL_HANDLE)
            return cc::error(cc::format("binding_group: could not create the sampler for '{}'", ns.name));
        write_sampler_descriptor(ctx, sampler, base + layout->descriptor_offset_of(slot, 0));
    }

    // Every binding must be provided — there is no default descriptor at this level.
    // A static sampler is the exception, since the layout supplies it.
    for (isize i = 0; i < bindings.size(); ++i)
    {
        if (sg::is_sampler(bindings[i].type))
        {
            if (layout->_slot_samplers[i] == VK_NULL_HANDLE && sampler_filled[i] == char(0))
                return cc::error(cc::format("binding_group: sampler '{}' was not provided", bindings[i].name));
        }
        else if (view_filled[i] == char(0))
            return cc::error(cc::format("binding_group: binding '{}' was not provided", bindings[i].name));
    }

    return vulkan_binding_group_handle(cc::move(group));
}

cc::result<vulkan_binding_group_handle> vulkan_binding_group::create_from_image(
    vulkan_context& ctx,
    vulkan_binding_group_layout_handle const& layout,
    cc::span<byte const> image,
    cc::vector<vulkan_buffer_handle> referenced,
    cc::vector<vulkan_texture_handle> referenced_textures,
    cc::vector<vulkan_hazard_view> hazard_views,
    cc::vector<vulkan_texture_hazard_view> texture_hazard_views,
    cc::vector<vulkan_array_binding> array_bindings)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_group_layout");
    CC_ASSERT(image.size() == layout->_size_in_bytes, "the descriptor image must be one layout-sized set");

    auto group = std::make_shared<vulkan_binding_group>();
    group->_ctx = &ctx;
    group->layout = layout;
    group->creation_epoch = ctx.current_epoch();

    {
        auto r = allocate_range(ctx, *layout, /*transient =*/false);
        CC_RETURN_IF_ERROR(r);
        group->range = cc::move(r.value());
    }

    // The whole point of the descriptor-buffer model: a snapshot of an arbitrarily large group is one copy, rather
    // than one descriptor write per element.
    if (!image.empty())
        cc::memcpy(ctx._descriptor_heap.mapped_at(group->range), image.data(), size_t(image.size()));

    group->referenced = cc::move(referenced);
    group->referenced_textures = cc::move(referenced_textures);
    group->hazard_views = cc::move(hazard_views);
    group->texture_hazard_views = cc::move(texture_hazard_views);
    group->array_bindings = cc::move(array_bindings);
    return vulkan_binding_group_handle(cc::move(group));
}
} // namespace sg::backend::vulkan

namespace sg::backend::vulkan
{
cc::result<vulkan_binding_group_handle> vulkan_context::create_vulkan_binding_group(
    sg::binding_group_layout_handle const& layout,
    cc::span<sg::named_view const> views,
    cc::span<sg::named_sampler const> samplers,
    sg::lifetime_scope scope)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_group_layout");
    auto vk_layout = std::dynamic_pointer_cast<vulkan_binding_group_layout const>(layout);
    CC_ASSERT(vk_layout != nullptr, "binding_group_layout is not a vulkan one");
    return vulkan_binding_group::create(*this, vk_layout, views, samplers, scope);
}
} // namespace sg::backend::vulkan
