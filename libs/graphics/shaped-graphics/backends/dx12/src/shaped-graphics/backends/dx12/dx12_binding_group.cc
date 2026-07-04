#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_view_desc.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/views.hh>

namespace sg::backend::dx12
{
cc::result<dx12_binding_group_handle> dx12_binding_group::create(dx12_context& ctx,
                                                                 dx12_binding_layout_handle layout,
                                                                 cc::span<sg::named_view const> views,
                                                                 sg::lifetime_scope scope)
{
    CC_ASSERT(layout != nullptr, "binding_group requires a binding_layout");

    auto group = std::make_shared<dx12_binding_group>();
    group->layout = layout;
    group->transient = scope == sg::lifetime_scope::transient;
    group->creation_epoch = ctx.current_epoch();

    // Persistent groups bump-allocate (live until teardown); transient groups ring-allocate (reclaimed
    // when their epoch retires).
    UINT const base = group->transient ? ctx._descriptor_heap.allocate_transient(layout->descriptor_count)
                                       : ctx._descriptor_heap.allocate_persistent(layout->descriptor_count);
    group->table_start = ctx._descriptor_heap.gpu_at(base);

    // Match each provided view to a layout slot by name, validate it, and create its descriptor.
    cc::vector<char> filled;
    for (cc::isize i = 0; i < layout->slots.size(); ++i)
        filled.push_back(char(0));

    for (auto const& nv : views)
    {
        cc::isize slot_index = -1;
        for (cc::isize i = 0; i < layout->slots.size(); ++i)
            if (layout->slots[i].binding.name == nv.name)
            {
                slot_index = i;
                break;
            }
        if (slot_index < 0)
            return cc::error(cc::format("binding_group: no binding named '{}' in the layout", nv.name));

        auto const& s = layout->slots[slot_index];
        if (!sg::accepts(s.binding.type, nv.view))
            return cc::error(
                cc::format("binding_group: the view bound to '{}' does not match its declared kind", nv.name));
        CC_ASSERT(filled[slot_index] == char(0), "binding_group: a binding was provided more than once");
        filled[slot_index] = char(1);

        create_buffer_view(ctx._device.Get(), nv.view, ctx._descriptor_heap.cpu_at(base + s.table_offset));
        if (nv.view.buffer)
            group->referenced.push_back(nv.view.buffer);
    }

    // Every declared binding must be provided (no null / default descriptors yet).
    for (cc::isize i = 0; i < filled.size(); ++i)
        if (filled[i] == char(0))
            return cc::error(cc::format("binding_group: binding '{}' was not provided", layout->slots[i].binding.name));

    return dx12_binding_group_handle(cc::move(group));
}
} // namespace sg::backend::dx12
