#pragma once

#include <blob-cache/blob_cache.hh>
#include <blob-cache/impl/cache_actor.hh>
#include <blob-cache/impl/singleflight.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>

/// What an acquire's continuations reach back into after the caller has let go of its blob_cache handle.
///
/// Shared rather than owned by the handle, because a handle destroyed mid-acquire would otherwise leave a running continuation pointing at a destroyed flight table.
/// The continuations hold a WEAK reference, so the core still dies with the last handle — they simply find it gone and skip the store rather than dereferencing a corpse.

namespace bcache::impl
{
struct cache_core
{
    cc::unique_ptr<cache_actor> actor;
    flight_table flights;

    cc::mutex<cache_stats> stats;
    cc::mutex<cache_limits> limits;

    cc::shared_async<cc::unit> opened;
    cc::atomic<bool> is_closed = {false};

    /// False for a cache built by create_disabled(), which has no actor at all.
    bool has_actor() const { return actor != nullptr; }
};

/// Mirrors one cache_stats counter into the recording, adding `n` to it.
///
/// Every counter bump in the library goes through here, so the recorded numbers and the in-process struct can never
/// disagree about what happened — hit rate is bcache's primary health signal, and until this existed the only way to
/// see it was to hold a live cache and ask.
///
/// **Total over cache_stats' counters**, and it asserts on a field it does not know: a stat name is a string literal
/// in the event descriptor, so the mapping cannot be computed and a new counter has to be added here by hand.
/// Asserting is what turns that into a compile-then-fail rather than a number that silently never appears.
void record_stat(i64 cache_stats::* field, i64 n);
} // namespace bcache::impl
