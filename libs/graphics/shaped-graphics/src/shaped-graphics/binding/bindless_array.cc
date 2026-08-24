#include "bindless_array.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <shaped-graphics/binding/staging_binding_group.hh>
#include <shaped-graphics/context/context.hh>

using namespace cc::primitive_defines;

sg::bindless_array sg::bindless_array::for_binding(context& ctx, staging_binding_group_handle group, cc::string_view name)
{
    CC_ASSERT(group != nullptr, "a bindless array needs a staging binding group");
    auto const slot = group->slot_of(name);
    CC_ASSERT(slot != binding_slot::invalid, "the group's layout has no binding of that name");
    CC_ASSERT(group->is_array(slot), "a bindless array needs an array binding (count >= 2, so elements can be vacant)");

    // The table starts empty, so the descriptors must too — and this is also what says the binding was set.
    group->unset_array(slot);
    auto const capacity = u32(group->array_size(slot));
    return bindless_array(std::make_shared<impl::bindless_array_state>(ctx, cc::move(group), slot, capacity));
}

sg::impl::bindless_array_state::bindless_array_state(context& ctx,
                                                     staging_binding_group_handle group,
                                                     binding_slot slot,
                                                     u32 capacity)
  : ctx(&ctx), group(cc::move(group)), slot(slot), table(capacity)
{
}

u32 sg::impl::bindless_array_state::_acquire(raw_view const& view, bool pin)
{
    // The table keys on the view itself, so resources participate by address and a hash collision resolves to
    // an inequality rather than a shared slot; the mapped entry holds the view, keeping that address valid.
    auto const e = ctx->current_epoch();
    auto const r = pin ? table.acquire_pinned(view, e, [&](u32 freed) { clear(freed); })
                       : table.acquire(view, e, [&](u32 freed) { clear(freed); });
    if (r.inserted)
        group->set_array_element(slot, int(r.index), view);
    return r.index;
}

void sg::impl::bindless_array_state::unpin(u32 index)
{
    table.unpin(index, ctx->current_epoch(), [&](u32 freed) { clear(freed); });
}

void sg::impl::bindless_array_state::clear(u32 index)
{
    group->unset_array_element(slot, int(index));
}

sg::bindless_element_handle sg::bindless_array_persistent_scope::acquire(raw_view const& view)
{
    auto const index = _state.acquire_pinned(view);

    // One element is pinned once: a second acquire of a view already held shares the existing handle, so the
    // refcount counts holders rather than acquires, and the slot frees exactly when the last one lets go.
    if (auto const* weak = _state.elements.get_ptr(index))
        if (auto existing = weak->lock())
            return existing;

    auto handle = std::shared_ptr<bindless_element>(new bindless_element(_state.shared_from_this(), index));
    _state.elements[index] = handle;
    return handle;
}

sg::bindless_element::~bindless_element()
{
    _state->elements.erase(_index);
    _state->unpin(_index);
}
