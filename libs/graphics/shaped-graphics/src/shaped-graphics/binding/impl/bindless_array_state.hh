#pragma once

#include <clean-core/container/map.hh>
#include <shaped-graphics/binding/impl/slot_table.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh>

namespace sg::impl
{
/// Everything one bindless array owns, held behind a shared_ptr so a persistent element can outlive the
/// `bindless_array` value that minted it.
///
/// That is the whole reason this is split out: `bindless_array` is a value a caller keeps in a container, while
/// a `bindless_element_handle` may be stored anywhere and released at any time — including after every array
/// naming this binding is gone.
/// Both hold the state, so a release is always safe and the descriptors it clears always exist.
///
/// Not thread-safe, like the staging group it writes to.
class bindless_array_state : public std::enable_shared_from_this<bindless_array_state>
{
public:
    bindless_array_state(context& ctx, staging_binding_group_handle group, binding_slot slot, u32 capacity);

    /// The slot for `view` this epoch, writing its descriptor on a mint.
    [[nodiscard]] u32 acquire(raw_view const& view) { return _acquire(view, false); }

    /// The same, with the slot pinned so no reclaim may take it until it is unpinned.
    [[nodiscard]] u32 acquire_pinned(raw_view const& view) { return _acquire(view, true); }

    /// Releases the pin on `index`.
    ///
    /// The slot is freed at once *unless* it was also acquired transiently this epoch, in which case it is only
    /// unpinned and the ordinary stale sweep takes it later.
    /// Freeing it early would pull the descriptor out from under an index already recorded into this epoch's
    /// work, which is what the reclaim rule exists to prevent.
    void unpin(u32 index);

    /// Clears a freed element's descriptor, so the table and the group never drift apart.
    void clear(u32 index);

    context* ctx = nullptr;
    staging_binding_group_handle group;
    binding_slot slot = binding_slot::invalid;
    slot_table<raw_view> table;

    /// The live handle per pinned slot, so acquiring a view that is already held hands back the same one
    /// rather than pinning the element a second time.
    /// Weak, because the handle's own refcount is what keeps the pin alive.
    cc::map<u32, std::weak_ptr<bindless_element>> elements;

private:
    /// The slot for `view`, mirroring a mint and every reclaim onto the group, which owns the descriptors.
    [[nodiscard]] u32 _acquire(raw_view const& view, bool pin);
};
} // namespace sg::impl
