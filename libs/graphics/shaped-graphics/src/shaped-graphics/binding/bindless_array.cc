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
    return bindless_array(ctx, cc::move(group), slot, capacity);
}

sg::bindless_array::bindless_array(context& ctx, staging_binding_group_handle group, binding_slot slot, u32 capacity)
  : _ctx(ctx), _group(cc::move(group)), _slot(slot), _table(capacity)
{
}

u32 sg::bindless_array::acquire(raw_view const& view)
{
    CC_ASSERT(!_locked, "no acquires while the bindless array is locked (unlock first)");

    // The table keys on the view itself, so resources participate by address and a hash collision resolves to
    // an inequality rather than a shared slot; the mapped entry holds the view, keeping that address valid.
    // Every mint and reclaim is mirrored onto the staging group, which owns the descriptors.
    auto const r
        = _table.acquire(view, _ctx.current_epoch(), [&](u32 freed) { _group->unset_array_element(_slot, int(freed)); });
    if (r.inserted)
        _group->set_array_element(_slot, int(r.index), view);
    return r.index;
}

void sg::bindless_array::lock()
{
    CC_ASSERT(!_locked, "the bindless array is already locked");
    _locked = true;
    _lock_epoch = _ctx.current_epoch();
}

void sg::bindless_array::unlock()
{
    CC_ASSERT(_locked, "unlock without a lock");
    CC_ASSERT(_ctx.current_epoch() == _lock_epoch, "lock and unlock must happen in the same epoch");
    _locked = false;
}
