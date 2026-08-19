#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_acceleration_structure.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_sampler.hh>
#include <shaped-graphics/backends/dx12/dx12_view_desc.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/binding/binding_group.hh>
#include <shaped-graphics/resource/views.hh>

namespace sg::backend::dx12
{
dx12_binding_group::~dx12_binding_group()
{
    // A persistent group returns its descriptor ranges to their heaps' free lists, deferred until its
    // last-using epoch retires (via a finalizer, mirroring buffer deletion). Transient ranges are reclaimed
    // by the rings, so nothing to free here.
    if (_ctx == nullptr || transient)
        return;
    if (table.is_empty() && sampler_table.is_empty())
        return;

    dx12_expiring_resource expiring;
    auto* const res_heap = &_ctx->_descriptor_heap;
    auto* const samp_heap = &_ctx->_sampler_heap;
    if (!table.is_empty())
        expiring.finalizers.push_back([res_heap, alloc = cc::move(table)]() mutable
                                      { res_heap->free_persistent(cc::move(alloc)); });
    if (!sampler_table.is_empty())
        expiring.finalizers.push_back([samp_heap, alloc = cc::move(sampler_table)]() mutable
                                      { samp_heap->free_persistent(cc::move(alloc)); });
    _ctx->schedule_deferred_deletion(cc::move(expiring));
}

cc::result<dx12_binding_group_handle> dx12_binding_group::create(dx12_context& ctx,
                                                                 dx12_binding_group_layout_handle const& layout,
                                                                 cc::span<sg::named_view const> views,
                                                                 cc::span<sg::named_sampler const> samplers,
                                                                 sg::lifetime_scope scope)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_group_layout");

    auto group = std::make_shared<dx12_binding_group>();
    group->_ctx = &ctx;
    group->layout = layout;
    group->transient = scope == sg::lifetime_scope::transient;
    group->creation_epoch = ctx.current_epoch();

    // Allocate a range in each heap the layout uses.
    // Persistent groups allocate from the free list, freed on release; transient groups ring-allocate, reclaimed collectively when their epoch retires.
    // Stored on the group so the destructor frees a persistent range even if creation fails partway.
    auto const alloc_table = [&](dx12_descriptor_heap& heap, int count) -> cc::result<dx12_descriptor_alloc>
    {
        if (count == 0)
            return dx12_descriptor_alloc{};
        dx12_descriptor_alloc a = group->transient ? heap.allocate_transient(count) : heap.allocate_persistent(count);
        // A persistent allocation of a non-empty table can come back empty when the fixed persistent region
        // is exhausted (bindless / many long-lived groups). Recoverable: report it.
        if (!group->transient && a.is_empty())
            return cc::error("binding_group: persistent descriptor heap exhausted (no free span fits the table)");
        return a;
    };

    {
        auto r = alloc_table(ctx._descriptor_heap, layout->descriptor_count);
        CC_RETURN_IF_ERROR(r);
        group->table = cc::move(r.value());
    }
    {
        auto r = alloc_table(ctx._sampler_heap, layout->sampler_descriptor_count);
        CC_RETURN_IF_ERROR(r);
        group->sampler_table = cc::move(r.value());
    }
    if (!group->table.is_empty())
        group->table_start = ctx._descriptor_heap.gpu_at(group->table.offset);
    if (!group->sampler_table.is_empty())
        group->sampler_table_start = ctx._sampler_heap.gpu_at(group->sampler_table.offset);

    // Match each provided view list to a view slot by name, validate it, and create its descriptors.
    // A scalar slot (count == 1) is auto-tracked: its view lands in the hazard vectors, declared at dispatch.
    // An array slot (count > 1) is not: its elements land in `array_bindings`, and the dispatching caller
    // declares the used elements via declare_array_*_access.
    cc::vector<char> view_filled;
    for (isize i = 0; i < layout->view_slots.size(); ++i)
        view_filled.push_back(char(0));

    int const view_base = group->table.offset;
    for (auto const& nv : views)
    {
        isize slot_index = -1;
        for (isize i = 0; i < layout->view_slots.size(); ++i)
            if (layout->view_slots[i].binding.name == nv.name)
            {
                slot_index = i;
                break;
            }
        if (slot_index < 0)
            return cc::error(cc::format("binding_group: no view binding named '{}' in the layout", nv.name));

        auto const& s = layout->view_slots[slot_index];
        bool const is_array = s.binding.count > 1;
        if (isize(nv.views.size()) != isize(s.binding.count))
            return cc::error(cc::format("binding_group: '{}' takes {} view(s), {} provided (an array binding takes "
                                        "exactly one per element; a vacant element is a null-handle view)",
                                        nv.name, s.binding.count, nv.views.size()));
        CC_ASSERT(view_filled[slot_index] == char(0), "binding_group: a binding was provided more than once");
        view_filled[slot_index] = char(1);

        auto array_binding = dx12_array_binding{.name = nv.name, .is_texture = false, .elements = {}};

        for (isize element = 0; element < nv.views.size(); ++element)
        {
            auto const& view = nv.views[element];
            if (!sg::accepts(s.binding.type, view))
                return cc::error(
                    cc::format("binding_group: element {} of '{}' does not match its declared kind", element, nv.name));

            auto const dst = ctx._descriptor_heap.cpu_at(view_base + s.table_offset + int(element));
            if (auto const* av = sg::try_as_tlas_view(view))
            {
                if (is_array)
                    return cc::error(cc::format("binding_group: '{}' — acceleration-structure arrays are not "
                                                "supported yet",
                                                nv.name));

                // A null handle is the null acceleration structure, not a mistake: it binds a valid descriptor that
                // every ray misses, so there is no storage to keep alive and nothing to declare a hazard on.
                auto dx_tlas = std::shared_ptr<dx12_tlas const>();
                if (av->tlas != nullptr)
                {
                    dx_tlas = std::dynamic_pointer_cast<dx12_tlas const>(av->tlas);
                    CC_ASSERT(dx_tlas != nullptr, "bound acceleration structure is not a dx12 tlas");
                }

                create_accel_view(ctx._device.Get(), dx_tlas.get(), dst);

                if (dx_tlas != nullptr)
                {
                    // The trace reads the AS storage buffer — record it (kept alive + declared accel_read at dispatch).
                    group->referenced.push_back(dx_tlas->_dx12_storage);
                    group->hazard_views.push_back({dx_tlas->_dx12_storage, sg::view_class::acceleration_structure});
                }
            }
            else if (auto const* tv = sg::try_as_texture_view(view))
            {
                if (tv->texture == nullptr && !is_array)
                    return cc::error(cc::format("binding_group: '{}' — a null texture view is only valid as a vacant "
                                                "array element",
                                                nv.name));

                create_texture_view(ctx._device.Get(), *tv, dst);
                auto dx = dx12_texture_handle();
                if (tv->texture != nullptr)
                {
                    dx = std::dynamic_pointer_cast<dx12_texture const>(tv->texture);
                    CC_ASSERT(dx != nullptr, "bound texture is not a dx12 texture");
                }
                if (is_array)
                {
                    array_binding.is_texture = true;
                    array_binding.elements.push_back({.buffer = {}, .texture = cc::move(dx), .range = tv->range});
                }
                else
                {
                    group->referenced_textures.push_back(cc::move(dx));
                    group->texture_hazard_views.push_back(
                        {group->referenced_textures.back(), tv->range, tv->access}); // → dispatch hazard declare
                }
            }
            else
            {
                auto const& bv = sg::as_buffer_view(view);
                if (bv.buffer == nullptr && !is_array && bv.access != sg::view_class::readonly
                    && bv.access != sg::view_class::readwrite)
                    return cc::error(cc::format("binding_group: '{}' — a null buffer view is not valid here", nv.name));

                create_buffer_view(ctx._device.Get(), bv, dst);
                auto dx = dx12_buffer_handle();
                if (bv.buffer != nullptr)
                {
                    dx = std::dynamic_pointer_cast<dx12_buffer const>(bv.buffer);
                    CC_ASSERT(dx != nullptr, "bound buffer is not a dx12 buffer");
                }
                if (is_array)
                {
                    array_binding.elements.push_back({.buffer = cc::move(dx), .texture = {}, .range = {}});
                }
                else if (dx != nullptr)
                {
                    group->referenced.push_back(cc::move(dx));
                    group->hazard_views.push_back(
                        {group->referenced.back(), bv.access}); // (buffer, access class) → dispatch hazard declare
                }
            }
        }

        if (is_array)
            group->array_bindings.push_back(cc::move(array_binding));
    }

    // Match each provided dynamic sampler to a sampler slot by name and create its descriptor.
    // Static samplers live in the root signature, so they must not be provided here.
    cc::vector<char> sampler_filled;
    for (isize i = 0; i < layout->sampler_slots.size(); ++i)
        sampler_filled.push_back(char(0));

    int const sampler_base = group->sampler_table.offset;
    for (auto const& ns : samplers)
    {
        isize slot_index = -1;
        for (isize i = 0; i < layout->sampler_slots.size(); ++i)
            if (layout->sampler_slots[i].binding.name == ns.name)
            {
                slot_index = i;
                break;
            }
        if (slot_index < 0)
            return cc::error(cc::format("binding_group: no dynamic sampler binding named '{}' in the layout (a static "
                                        "sampler must not be supplied per group)",
                                        ns.name));
        CC_ASSERT(sampler_filled[slot_index] == char(0), "binding_group: a sampler was provided more than once");
        CC_ASSERT(layout->sampler_slots[slot_index].binding.count == 1, "dynamic sampler arrays are not supported "
                                                                        "yet");
        sampler_filled[slot_index] = char(1);

        D3D12_SAMPLER_DESC const desc = to_d3d12_sampler_desc(ns.sampler);
        ctx._device->CreateSampler(
            &desc, ctx._sampler_heap.cpu_at(sampler_base + layout->sampler_slots[slot_index].table_offset));
    }

    // Every declared view and dynamic sampler must be provided (no null / default descriptors yet).
    for (isize i = 0; i < view_filled.size(); ++i)
        if (view_filled[i] == char(0))
            return cc::error(
                cc::format("binding_group: binding '{}' was not provided", layout->view_slots[i].binding.name));
    for (isize i = 0; i < sampler_filled.size(); ++i)
        if (sampler_filled[i] == char(0))
            return cc::error(
                cc::format("binding_group: sampler '{}' was not provided", layout->sampler_slots[i].binding.name));

    return dx12_binding_group_handle(cc::move(group));
}
} // namespace sg::backend::dx12
