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
/// When the table is full, EVERY slot not acquired in the current epoch is reclaimed at once — the mint
/// dirties the mirror and forces a group recreation anyway, so there is nothing to save by evicting less.
/// If every slot was acquired in the current epoch, the working set exceeds the capacity and acquire asserts.
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
    /// A miss takes a free slot; a full table first reclaims every slot not acquired in epoch `e` (the mint
    /// forces a group recreation anyway) and asserts that freed at least one.
    /// `key` must identify `view`: two calls with the same key must describe the same view.
    [[nodiscard]] u32 acquire(u64 key, sg::raw_view view, sg::epoch e)
    {
        if (auto const* slot = _by_key.get_ptr(key))
        {
            _entries[*slot].last_acquired = e;
            return *slot;
        }

        if (_free.empty())
            _reclaim_stale(e);

        auto const slot = _free.back();
        _free.pop_back();

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
    /// Frees every occupied slot not acquired in epoch `e`; slots acquired in `e` are never victims.
    /// Each freed slot's key is erased with it, so a stale key can never resolve to a later occupant.
    void _reclaim_stale(sg::epoch e)
    {
        for (isize i = 0; i < _entries.size(); ++i)
        {
            if (!_entries[i].occupied || _entries[i].last_acquired == e)
                continue;
            _by_key.erase(_entries[i].key);
            _entries[i] = {};
            _free.push_back(u32(i));
        }
        CC_ASSERT(!_free.empty(), "bindless slot table is full: every slot was acquired this epoch (the working set "
                                  "exceeds the configured capacity)");
    }

    cc::vector<entry> _entries;
    cc::map<u64, u32> _by_key;
    cc::vector<u32> _free;
    bool _dirty = false;
};
