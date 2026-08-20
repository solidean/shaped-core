#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-viewer/fwd.hh>

/// A fixed-capacity table of bindless slots for one resource category — the key → element-index identity
/// map behind one array binding of the manager's staging_binding_group (see resources/bindless_manager.hh).
///
/// `acquire` is keyed by view identity: re-acquiring the same view returns the same slot, so an unchanged
/// working set never touches a descriptor.
/// A slot handed out is only valid for the epoch it was acquired in — the table never reclaims a slot
/// acquired in the current epoch, so within one epoch every handed-out slot stays live.
/// When the table is full, EVERY slot not acquired in the current epoch is reclaimed at once — the mint
/// dirties the group and forces a snapshot anyway, so there is nothing to save by evicting less.
/// If every slot was acquired in the current epoch, the working set exceeds the capacity and acquire asserts.
///
/// The table owns only the identity mapping; the descriptors, and the resource lifetimes behind them, live
/// in the staging group.
/// The owner keeps the two in step through acquire's contract: mirror every `inserted` result and every
/// `on_reclaimed` call onto the group, which is also what keeps a key's raw pointer from being reused while
/// the key is mapped.
class sv::impl::slot_table
{
public:
    /// `capacity` slots, all free; must be > 0.
    explicit slot_table(u32 capacity) : _entries(), _by_key(), _free()
    {
        CC_ASSERT(capacity > 0, "a slot table needs at least one slot");
        for (u32 i = 0; i < capacity; ++i)
        {
            _entries.push_back({});
            _free.push_back(capacity - 1 - i); // pop_back hands out slot 0 first
        }
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
    [[nodiscard]] acquired acquire(u64 key, sg::epoch e, auto&& on_reclaimed)
    {
        if (auto const* slot = _by_key.get_ptr(key))
        {
            _entries[*slot].last_acquired = e;
            return {.index = *slot, .inserted = false};
        }

        if (_free.empty())
            _reclaim_stale(e, on_reclaimed);

        auto const slot = _free.back();
        _free.pop_back();

        _entries[slot] = {.key = key, .last_acquired = e, .occupied = true};
        _by_key[key] = slot;
        return {.index = slot, .inserted = true};
    }

    [[nodiscard]] isize capacity() const { return _entries.size(); }

    [[nodiscard]] isize occupied_count() const { return _entries.size() - _free.size(); }

private:
    /// One slot's identity: the key mapped to it, and the epoch that last acquired it.
    struct entry
    {
        u64 key = 0;
        sg::epoch last_acquired = sg::epoch::invalid;
        bool occupied = false;
    };

    /// Frees every occupied slot not acquired in epoch `e`; slots acquired in `e` are never victims.
    /// Each freed slot's key is erased with it, so a stale key can never resolve to a later occupant.
    void _reclaim_stale(sg::epoch e, auto&& on_reclaimed)
    {
        for (isize i = 0; i < _entries.size(); ++i)
        {
            if (!_entries[i].occupied || _entries[i].last_acquired == e)
                continue;
            _by_key.erase(_entries[i].key);
            _entries[i] = {};
            _free.push_back(u32(i));
            on_reclaimed(u32(i));
        }
        CC_ASSERT(!_free.empty(), "bindless slot table is full: every slot was acquired this epoch (the working set "
                                  "exceeds the configured capacity)");
    }

    cc::vector<entry> _entries;
    cc::map<u64, u32> _by_key;
    cc::vector<u32> _free;
};
