#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::impl
{
/// A fixed-capacity table of bindless slots — the key → element-index identity map behind one array binding
/// of a staging_binding_group (see binding/bindless_array.hh).
/// `Key` is the identity, and must hash and compare: bindless_array keys on `raw_view` itself, so a hash
/// collision cannot make two different views share a slot.
///
/// `acquire` is keyed by that identity: re-acquiring the same key returns the same slot, so an unchanged
/// working set never touches a descriptor.
/// A slot handed out is only valid for the epoch it was acquired in — the table never reclaims a slot
/// acquired in the current epoch, so within one epoch every handed-out slot stays live.
/// When the table is full, EVERY slot not acquired in the current epoch is reclaimed at once — the mint
/// dirties the group and forces a snapshot anyway, so there is nothing to save by evicting less.
/// If every slot was acquired in the current epoch, the working set exceeds the capacity and acquire asserts.
///
/// A slot may also be **pinned**, which is what outlives an epoch: the stale sweep never reclaims a pinned
/// slot, whatever epoch last acquired it.
/// `unpin` frees it again at once — so an index read after its pin is gone resolves to a vacant descriptor
/// rather than to whatever moved in, which is what makes a stale persistent index fail rather than silently
/// name another resource.
/// The one exception is a slot also acquired transiently in the current epoch: that one is only unpinned, and
/// the ordinary sweep takes it later, because freeing it would pull the descriptor out from under an index
/// already recorded into this epoch's work.
///
/// The table owns the identity mapping and keeps each mapped key alive; the descriptors, and the resources
/// they reference, live in the staging group.
/// The owner keeps the two in step through acquire's contract: mirror every `inserted` result and every
/// `on_reclaimed` call onto the group.
template <class Key>
class slot_table
{
public:
    /// `capacity` slots, all free; must be > 0.
    explicit slot_table(u32 capacity) : _entries(), _by_key(), _free()
    {
        CC_ASSERT(capacity > 0, "a slot table needs at least one slot");
        _entries.resize_to_defaulted(capacity);
        _free.reserve(capacity);
        for (u32 i = 0; i < capacity; ++i)
            _free.push_back(capacity - 1 - i); // pop_back hands out slot 0 first
    }

    /// What one acquire resolved to: the slot, and whether it was freshly minted (→ write the descriptor).
    struct acquired
    {
        u32 index = 0;
        bool inserted = false;
    };

    /// The slot for `key`, minting one on a miss.
    /// A hit re-stamps the slot's epoch; nothing else changes.
    /// A miss takes a free slot; a full table first reclaims every slot not acquired in epoch `e`, calling
    /// `on_reclaimed(u32 slot)` for each so the owner clears its descriptor, and asserts that freed at least one.
    [[nodiscard]] acquired acquire(Key const& key, epoch e, auto&& on_reclaimed)
    {
        return _acquire(key, e, false, on_reclaimed);
    }

    /// The slot for `key`, minting one on a miss, and pinned.
    /// A pinned slot survives every stale sweep until `unpin`, so its index may be written somewhere that
    /// outlives the epoch.
    /// Pinning a key that is already pinned returns the same slot and changes nothing.
    [[nodiscard]] acquired acquire_pinned(Key const& key, epoch e, auto&& on_reclaimed)
    {
        return _acquire(key, e, true, on_reclaimed);
    }

    /// Unpins `slot`, which must be occupied and pinned, and frees it unless it was also acquired in epoch `e`.
    /// A freed slot calls `on_reclaimed(slot)`, so the owner clears its descriptor.
    void unpin(u32 slot, epoch e, auto&& on_reclaimed)
    {
        CC_ASSERT(slot < u32(_entries.size()), "slot out of range");
        auto& entry = _entries[slot];
        CC_ASSERT(entry.occupied, "unpin of a free slot");
        CC_ASSERT(entry.pinned, "unpin without a matching pin");

        entry.pinned = false;
        if (entry.last_acquired != e)
            _free_slot(slot, on_reclaimed);
    }

    [[nodiscard]] isize capacity() const { return _entries.size(); }

    [[nodiscard]] isize occupied_count() const { return _entries.size() - _free.size(); }

    /// How many slots are currently pinned — the floor the transient working set has to fit above.
    [[nodiscard]] isize pinned_count() const
    {
        auto n = isize(0);
        for (auto const& e : _entries)
            if (e.occupied && e.pinned)
                ++n;
        return n;
    }

    [[nodiscard]] bool is_pinned(u32 slot) const
    {
        CC_ASSERT(slot < u32(_entries.size()), "slot out of range");
        return _entries[slot].occupied && _entries[slot].pinned;
    }

private:
    /// One slot's identity: the key mapped to it, the epoch that last acquired it, and whether it is pinned.
    struct entry
    {
        Key key = {};
        epoch last_acquired = epoch::invalid;
        bool pinned = false; ///< exempts the slot from the stale sweep
        bool occupied = false;
    };

    [[nodiscard]] acquired _acquire(Key const& key, epoch e, bool pin, auto&& on_reclaimed)
    {
        if (auto const* slot = _by_key.get_ptr(key))
        {
            _entries[*slot].last_acquired = e;
            _entries[*slot].pinned |= pin;
            return {.index = *slot, .inserted = false};
        }

        if (_free.empty())
            _reclaim_stale(e, on_reclaimed);

        auto const slot = _free.pop_back();

        _entries[slot] = {.key = key, .last_acquired = e, .pinned = pin, .occupied = true};
        _by_key[key] = slot;
        return {.index = slot, .inserted = true};
    }

    void _free_slot(u32 slot, auto&& on_reclaimed)
    {
        _by_key.erase(_entries[slot].key);
        _entries[slot] = {};
        _free.push_back(slot);
        on_reclaimed(slot);
    }

    /// Frees every occupied slot not acquired in epoch `e`; slots acquired in `e`, and pinned slots, are never
    /// victims.
    /// Each freed slot's key is erased with it, so a stale key can never resolve to a later occupant.
    void _reclaim_stale(epoch e, auto&& on_reclaimed)
    {
        for (isize i = 0; i < _entries.size(); ++i)
        {
            if (!_entries[i].occupied || _entries[i].last_acquired == e || _entries[i].pinned)
                continue;
            _free_slot(u32(i), on_reclaimed);
        }
        CC_ASSERT(!_free.empty(), "bindless slot table is full: every slot is pinned or was acquired this epoch (the "
                                  "working set, plus what is pinned, exceeds the configured capacity)");
    }

    cc::vector<entry> _entries;
    cc::map<Key, u32> _by_key;
    cc::vector<u32> _free;
};
} // namespace sg::impl
