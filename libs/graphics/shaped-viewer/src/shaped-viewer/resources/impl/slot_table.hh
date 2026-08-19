#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/views.hh>
#include <shaped-viewer/fwd.hh>

/// A fixed-capacity table of bindless slots for one resource category — the CPU mirror of one bindless
/// binding array (see resources/bindless_manager.hh).
///
/// `acquire` is keyed by view identity: re-acquiring the same view returns the same slot without touching
/// the mirror, which is what lets an unchanged frame skip the group reupload entirely.
/// A slot handed out is only valid for the epoch it was acquired in — the table never reclaims a slot
/// acquired in the current epoch, so within one epoch every handed-out slot stays live.
/// When the table is full, the least-recently-acquired slot is reclaimed; if every slot was acquired in the
/// current epoch, the working set exceeds the capacity and acquire asserts.
///
/// The occupied entry holds the view (and thereby the resource's handle) alive while its key is mapped, so a
/// key's raw pointer cannot be reused by a new resource while the entry lives.
class sv::impl::slot_table
{
public:
    /// `capacity` slots, all free; must be > 0.
    explicit slot_table(u32 capacity) : _entries(), _by_key(), _free(), _dirty(false)
    {
        CC_ASSERT(capacity > 0, "a slot table needs at least one slot");
        for (u32 i = 0; i < capacity; ++i)
        {
            _entries.push_back({});
            _free.push_back(capacity - 1 - i); // pop_back hands out slot 0 first
        }
    }

    /// The slot for `key`, minting one on a miss.
    /// A hit re-stamps the slot's epoch and leaves the mirror untouched (not dirty).
    /// A miss takes a free slot, else reclaims the least-recently-acquired one — never a slot acquired in
    /// epoch `e` — and marks the table dirty.
    /// `key` must identify `view`: two calls with the same key must describe the same view.
    [[nodiscard]] u32 acquire(u64 key, sg::raw_view view, sg::epoch e)
    {
        if (auto const* slot = _by_key.get_ptr(key))
        {
            _entries[*slot].last_acquired = e;
            return *slot;
        }

        auto slot = u32(0);
        if (!_free.empty())
        {
            slot = _free.back();
            _free.pop_back();
        }
        else
        {
            slot = _reclaim_lru(e);
        }

        _entries[slot] = {.view = cc::move(view), .key = key, .last_acquired = e, .occupied = true};
        _by_key[key] = slot;
        _dirty = true;
        return slot;
    }

    /// Whether the mirror changed since the last clear_dirty — the group-recreate trigger.
    [[nodiscard]] bool dirty() const { return _dirty; }

    void clear_dirty() { _dirty = false; }

    /// One slot of the mirror; `view` is meaningful only while `occupied`.
    struct entry
    {
        sg::raw_view view;
        u64 key = 0;
        sg::epoch last_acquired = sg::epoch::invalid;
        bool occupied = false;
    };

    /// The full mirror, slot-indexed — what the owner turns into a `named_view`'s element list.
    [[nodiscard]] cc::span<entry const> entries() const { return _entries; }

    [[nodiscard]] isize capacity() const { return _entries.size(); }

    [[nodiscard]] isize occupied_count() const { return _entries.size() - _free.size(); }

private:
    /// The occupied slot with the oldest `last_acquired`; a slot acquired in `e` is never a victim.
    /// Erases the victim's key before overwriting, so a stale key can never resolve to the new occupant.
    [[nodiscard]] u32 _reclaim_lru(sg::epoch e)
    {
        auto victim = isize(-1);
        for (isize i = 0; i < _entries.size(); ++i)
        {
            if (_entries[i].last_acquired == e)
                continue;
            if (victim < 0 || u64(_entries[i].last_acquired) < u64(_entries[victim].last_acquired))
                victim = i;
        }
        CC_ASSERT(victim >= 0, "bindless slot table is full: every slot was acquired this epoch (the working set "
                               "exceeds the configured capacity)");
        _by_key.erase(_entries[victim].key);
        return u32(victim);
    }

    cc::vector<entry> _entries;
    cc::map<u64, u32> _by_key;
    cc::vector<u32> _free;
    bool _dirty = false;
};
