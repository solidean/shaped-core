#pragma once

#include <clean-core/bytes/hash128.hh> // cc::hash128 (the content key)
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh> // sg::epoch
#include <shaped-viewer/fwd.hh>

namespace sv::impl
{
/// A small LRU resource pool: mints strongly-typed ids for records, tracks each record's byte size and the
/// epoch it was last used, and evicts on two triggers — an idle timeout (unused for more than
/// `max_idle_epochs` epochs) and a byte budget (over `max_bytes`, drop least-recently-used first).
///
/// Eviction is safe: a record is a bundle of sg handles, so dropping one just defers the GPU free until the resource is no longer in flight.
/// Anything still referencing it — a TLAS holding a BLAS — keeps it alive regardless.
/// Evicting an id a live scene still names is a caller error, so size the budget for the working set.
/// The idle timeout and LRU order are chosen so an actively-used record is never the victim.
///
/// This is the generic id-pool the viewer's concrete managers (mesh, material) build on — the reusable core
/// that a bare `cc::map` per resource type was standing in for.
///
/// It is content-addressed: each record is inserted under a caller-supplied `cc::hash128` content hash, and
/// `find_by_hash` resolves that hash back to a resident id in O(1). The pool never hashes anything itself —
/// hashes come from outside — so an acquire is O(1) on the happy path and touches the data only on a miss.
/// Collisions are resolved by hash alone, with no content compare, which is what the 128-bit width buys.
template <class Id, class Record>
class lru_pool
{
public:
    /// `max_bytes` <= 0 disables the byte budget; `max_idle_epochs` < 0 disables the idle timeout.
    void set_limits(isize max_bytes, isize max_idle_epochs)
    {
        _max_bytes = max_bytes;
        _max_idle_epochs = max_idle_epochs;
    }

    /// Reclaim, then advance to epoch `e`.
    /// Evicts everything idle past the timeout and, if still over the byte budget, the least-recently-used records.
    /// That is judged against the *just-finished* frame's usage, so the frame about to run keeps its working set.
    /// Call once at the start of each frame.
    void begin_frame(sg::epoch e)
    {
        _evict_idle();
        _enforce_budget();
        _epoch = e;
    }

    /// The record for `id`, or null if absent.
    /// Marks it used this epoch (an LRU touch).
    [[nodiscard]] Record const* get_ptr(Id id)
    {
        auto* const e = _entries.get_ptr(id);
        if (e == nullptr)
            return nullptr;
        e->last_used = _epoch;
        return &e->record;
    }

    /// The record for `id`, which must exist.
    /// Marks it used this epoch.
    [[nodiscard]] Record const& get(Id id)
    {
        auto const* const r = get_ptr(id);
        CC_ASSERT(r != nullptr, "lru_pool::get: unknown id");
        return *r;
    }

    [[nodiscard]] bool contains(Id id) const { return _entries.contains(id); }
    [[nodiscard]] isize count() const { return _entries.size(); }
    [[nodiscard]] isize used_bytes() const { return _total_bytes; }

    /// Drop `id` now if present; returns whether it was there.
    bool evict(Id id) { return _remove(id); }

protected:
    /// The resident id for content hash `hash`, or null if nothing with that hash is resident (never inserted,
    /// or evicted since). Marks a hit used this epoch (an LRU touch), so the acquire happy path keeps it alive.
    [[nodiscard]] cc::optional<Id> find_by_hash(cc::hash128 hash)
    {
        auto const* const idp = _by_hash.get_ptr(hash);
        if (idp == nullptr)
            return {};
        auto* const e = _entries.get_ptr(*idp);
        CC_ASSERT(e != nullptr, "lru_pool: hash index names a missing entry");
        e->last_used = _epoch;
        return *idp;
    }

    /// Store a freshly-built `record` of `size_in_bytes` under content `hash`, mint its id, stamp it used this epoch, then enforce the byte budget.
    /// Returns the new id.
    /// `hash` must not already be resident — call `find_by_hash` first.
    [[nodiscard]] Id insert(cc::hash128 hash, Record record, isize size_in_bytes)
    {
        CC_ASSERT(!_by_hash.contains(hash), "lru_pool::insert: content hash already resident — find_by_hash first");
        auto const id = Id(_next++);
        _entries[id]
            = entry{.record = cc::move(record), .size_in_bytes = size_in_bytes, .last_used = _epoch, .hash = hash};
        _by_hash[hash] = id;
        _total_bytes += size_in_bytes;
        _enforce_budget();
        return id;
    }

private:
    struct entry
    {
        Record record;
        isize size_in_bytes = 0;
        sg::epoch last_used = sg::epoch(0);
        cc::hash128 hash;
    };

    /// How many epochs `b` is behind `a` (a >= b by construction, so no underflow).
    [[nodiscard]] static u64 epochs_behind(sg::epoch a, sg::epoch b) { return u64(a) - u64(b); }

    bool _remove(Id id)
    {
        auto* const e = _entries.get_ptr(id);
        if (e == nullptr)
            return false;
        _total_bytes -= e->size_in_bytes;
        _by_hash.erase(e->hash);
        _entries.erase(id);
        return true;
    }

    void _evict_idle()
    {
        if (_max_idle_epochs < 0)
            return;
        auto stale = cc::vector<Id>();
        for (auto const& [id, e] : _entries)
            if (epochs_behind(_epoch, e.last_used) > u64(_max_idle_epochs))
                stale.push_back(id);
        for (auto const id : stale)
            _remove(id);
    }

    void _enforce_budget()
    {
        if (_max_bytes <= 0)
            return;
        while (_total_bytes > _max_bytes)
        {
            // Drop the least-recently-used record — but never one used this epoch (this frame's working set).
            auto victim = cc::optional<Id>();
            auto victim_age = u64(0);
            for (auto const& [id, e] : _entries)
            {
                if (e.last_used == _epoch)
                    continue;
                auto const age = epochs_behind(_epoch, e.last_used);
                if (!victim.has_value() || age > victim_age)
                {
                    victim = id;
                    victim_age = age;
                }
            }
            if (!victim.has_value())
                break; // everything left is in this frame's working set
            _remove(victim.value());
        }
    }

    cc::map<Id, entry> _entries;
    cc::map<cc::hash128, Id> _by_hash; // content hash -> id, kept in lockstep with _entries for O(1) acquire
    u32 _next = 0;                     // mints upward from 0; the id's ::invalid is u32(-1), never reached
    isize _total_bytes = 0;
    sg::epoch _epoch = sg::epoch(0);
    isize _max_bytes = 0;        // <= 0 => unlimited
    isize _max_idle_epochs = -1; // <  0 => never idle-evict
};
} // namespace sv::impl
