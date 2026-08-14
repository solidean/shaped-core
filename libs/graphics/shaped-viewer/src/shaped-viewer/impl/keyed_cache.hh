#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <shaped-viewer/fwd.hh>

namespace sv::impl
{
/// When a keyed_cache reclaims, and how much it is allowed to hold.
///
/// The two idle thresholds are the point of the type: `max_idle_frames_payload` releases the expensive part of a record while keeping its identity,
/// and `max_idle_frames_entry` drops the identity too.
/// A view's accumulation target is megabytes and its camera is a few dozen bytes, so they must not expire on the same schedule.
///
/// A negative threshold disables it, as does a budget of zero or less.
/// With both thresholds enabled, `max_idle_frames_payload` must not exceed `max_idle_frames_entry` — an entry cannot outlive its own removal.
struct keyed_cache_limits
{
    /// release the record, keep the entry
    /// Only entries that declared a size through `set_payload_bytes` are demoted — a record owning nothing has nothing to release.
    i64 max_idle_frames_payload = -1;

    /// drop the entry outright
    i64 max_idle_frames_entry = -1;

    /// over this, release payloads least-recently-used first
    isize max_payload_bytes = 0;

    /// over this, drop entries least-recently-used first
    isize max_entries = 0;
};

/// A cache keyed by an identity the caller owns, reclaimed on a frame clock the caller ticks.
///
/// This is the counterpart to `lru_pool` for state that is *named* rather than content-addressed: a key stays stable
/// while everything under it changes every frame, which is exactly a `view_id` and its camera, its placement and its accumulator.
/// `lru_pool` cannot serve that — it mints its own ids and resolves them through a content hash.
///
/// Reclamation runs in `begin_frame`, against the *just-finished* frame's usage, so the frame about to run keeps its working set.
/// Both budget passes skip anything already used this tick for the same reason.
///
/// The owner must call `begin_frame` before the first `get_or_create`, so every entry is stamped on the same clock from the start.
/// Two caches that reclaim on the same schedule must be fed the same tick value — mixing clocks silently makes one threshold mean something else.
///
/// A record counts as holding a payload exactly while it has declared bytes through `set_payload_bytes`.
/// That is what the release hook fires on and what demotion looks at, so a record owning something reclaimable must declare it.
/// The hook therefore runs exactly once per payload, whichever path drops it.
template <class Key, class Record>
class keyed_cache
{
public:
    void set_limits(keyed_cache_limits const& limits)
    {
        CC_ASSERT(limits.max_idle_frames_payload < 0 || limits.max_idle_frames_entry < 0
                      || limits.max_idle_frames_payload <= limits.max_idle_frames_entry,
                  "keyed_cache: a payload cannot outlive its entry");
        _limits = limits;
    }

    [[nodiscard]] keyed_cache_limits const& limits() const { return _limits; }

    /// Reclaim, then advance to `tick`.
    /// `on_release(Key, Record&)` runs for every payload dropped, by demotion or by eviction, and the record is reset right after it.
    /// So a record holding GPU handles frees them there, exactly once, whichever path dropped it.
    template <class OnRelease>
    void begin_frame(u64 tick, OnRelease&& on_release)
    {
        _demote_idle(on_release);
        _evict_idle(on_release);
        _enforce_payload_budget(on_release);
        _enforce_entry_budget(on_release);
        _tick = tick;
    }

    /// Reclaim and advance without a release hook — for records that own nothing.
    void begin_frame(u64 tick)
    {
        begin_frame(tick, [](Key, Record&) {});
    }

    /// The record for `key`, default-constructed on first use.
    /// Marks it used this tick.
    [[nodiscard]] Record& get_or_create(Key key)
    {
        auto& e = _entries[key];
        e.last_used = _tick;
        return e.record;
    }

    /// The record for `key`, or null if absent.
    /// Marks a hit used this tick.
    [[nodiscard]] Record* find(Key key)
    {
        auto* const e = _entries.get_ptr(key);
        if (e == nullptr)
            return nullptr;
        e->last_used = _tick;
        return &e->record;
    }

    /// The record for `key` without touching it — for hit-tests and asserts, which must not keep a view alive.
    [[nodiscard]] Record const* peek(Key key) const
    {
        auto const* const e = _entries.get_ptr(key);
        return e == nullptr ? nullptr : &e->record;
    }

    /// Mark `key` used this tick without creating it; returns whether it was there.
    bool touch(Key key)
    {
        auto* const e = _entries.get_ptr(key);
        if (e == nullptr)
            return false;
        e->last_used = _tick;
        return true;
    }

    /// What this record costs against `max_payload_bytes`.
    /// A cache whose records own no memory never calls this and leaves every entry at zero.
    void set_payload_bytes(Key key, isize bytes)
    {
        auto* const e = _entries.get_ptr(key);
        CC_ASSERT(e != nullptr, "keyed_cache::set_payload_bytes: unknown key");
        _total_bytes += bytes - e->payload_bytes;
        e->payload_bytes = bytes;
    }

    [[nodiscard]] bool contains(Key key) const { return _entries.contains(key); }
    [[nodiscard]] isize count() const { return _entries.size(); }
    [[nodiscard]] isize payload_bytes() const { return _total_bytes; }
    [[nodiscard]] u64 frame_index() const { return _tick; }

    /// How many ticks ago `key` was last used; 0 if it was used this tick.
    /// `key` must exist.
    [[nodiscard]] u64 idle_frames(Key key) const
    {
        auto const* const e = _entries.get_ptr(key);
        CC_ASSERT(e != nullptr, "keyed_cache::idle_frames: unknown key");
        return _frames_behind(_tick, e->last_used);
    }

    /// Visit every live entry as `f(Key, Record&)`, in unspecified order.
    /// Does not touch anything, so a sweep cannot accidentally keep the whole cache alive.
    template <class F>
    void for_each(F&& f)
    {
        for (auto [key, e] : _entries) // cc::map yields a proxy by value; its members are the live references
            f(key, e.record);
    }

    template <class F>
    void for_each(F&& f) const
    {
        for (auto const& [key, e] : _entries)
            f(key, e.record);
    }

    /// Drop `key` now if present, releasing its payload first; returns whether it was there.
    template <class OnRelease>
    bool evict(Key key, OnRelease&& on_release)
    {
        auto* const e = _entries.get_ptr(key);
        if (e == nullptr)
            return false;
        _release(key, *e, on_release);
        _entries.erase(key);
        return true;
    }

    /// Drop everything, releasing each payload exactly once.
    template <class OnRelease>
    void clear(OnRelease&& on_release)
    {
        for (auto [key, e] : _entries) // cc::map yields a proxy by value; its members are the live references
            _release(key, e, on_release);
        _entries.clear();
        _total_bytes = 0;
    }

private:
    struct entry
    {
        Record record;
        u64 last_used = 0;
        isize payload_bytes = 0;
    };

    /// How many ticks `b` is behind `a` (a >= b by construction, so no underflow).
    [[nodiscard]] static u64 _frames_behind(u64 a, u64 b) { return a - b; }

    /// Release `e`'s payload if it still holds one, so demotion followed by eviction fires the hook once, not twice.
    template <class OnRelease>
    void _release(Key key, entry& e, OnRelease& on_release)
    {
        if (e.payload_bytes <= 0)
            return;
        on_release(key, e.record);
        e.record = Record{};
        _total_bytes -= e.payload_bytes;
        e.payload_bytes = 0;
    }

    template <class OnRelease>
    void _demote_idle(OnRelease& on_release)
    {
        if (_limits.max_idle_frames_payload < 0)
            return;
        for (auto [key, e] : _entries)
            if (e.payload_bytes > 0 && _frames_behind(_tick, e.last_used) > u64(_limits.max_idle_frames_payload))
                _release(key, e, on_release);
    }

    template <class OnRelease>
    void _evict_idle(OnRelease& on_release)
    {
        if (_limits.max_idle_frames_entry < 0)
            return;
        auto stale = cc::vector<Key>();
        for (auto const& [key, e] : _entries)
            if (_frames_behind(_tick, e.last_used) > u64(_limits.max_idle_frames_entry))
                stale.push_back(key);
        for (auto const key : stale)
            (void)evict(key, on_release);
    }

    /// The least-recently-used entry matching `pred`, or none if every candidate is in this tick's working set.
    template <class Pred>
    [[nodiscard]] cc::optional<Key> _oldest(Pred pred) const
    {
        auto victim = cc::optional<Key>();
        auto victim_age = u64(0);
        for (auto const& [key, e] : _entries)
        {
            if (e.last_used == _tick || !pred(e))
                continue;
            auto const age = _frames_behind(_tick, e.last_used);
            if (!victim.has_value() || age > victim_age)
            {
                victim = key;
                victim_age = age;
            }
        }
        return victim;
    }

    template <class OnRelease>
    void _enforce_payload_budget(OnRelease& on_release)
    {
        if (_limits.max_payload_bytes <= 0)
            return;
        while (_total_bytes > _limits.max_payload_bytes)
        {
            auto const victim = _oldest([](entry const& e) { return e.payload_bytes > 0; });
            if (!victim.has_value())
                break; // everything still holding memory is in this tick's working set
            _release(victim.value(), *_entries.get_ptr(victim.value()), on_release);
        }
    }

    template <class OnRelease>
    void _enforce_entry_budget(OnRelease& on_release)
    {
        if (_limits.max_entries <= 0)
            return;
        while (_entries.size() > _limits.max_entries)
        {
            auto const victim = _oldest([](entry const&) { return true; });
            if (!victim.has_value())
                break; // everything left is in this tick's working set
            (void)evict(victim.value(), on_release);
        }
    }

    cc::map<Key, entry> _entries;
    u64 _tick = 0;
    isize _total_bytes = 0;
    keyed_cache_limits _limits;
};

/// Frames a view may go unseen before its identity is dropped.
/// Both per-view caches read this, so a view cannot survive in one and vanish from the other.
constexpr i64 view_idle_frames = 240;

/// Frames a view may go unseen before its GPU payload is released, its identity surviving.
/// Much shorter than `view_idle_frames`: an accumulation target is megabytes, a camera is not.
constexpr i64 view_payload_idle_frames = 60;
} // namespace sv::impl
