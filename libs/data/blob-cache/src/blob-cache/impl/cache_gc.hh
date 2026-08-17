#pragma once

#include <babel-serializer/data/sqlite.hh>
#include <blob-cache/blob_cache.hh>
#include <blob-cache/impl/cache_rows.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>

/// Reclamation: what gets thrown away, in what order, and how the work is cut into slices.
///
/// **The eviction score is not part of any contract.**
/// The database stores primitive signals — created_at, accessed_at, expires_at, compute_secs, object size — and this file is the only place that turns them into an order.
/// It may be replaced wholesale without touching the schema.
///
/// Today it is cost-aware value density, aged:
///
///     cost  = compute_secs > 0 ? compute_secs : default
///     score = (cost / max(size, 4096)) / (1 + (now - accessed_at) / half_life)
///
/// evaluated ascending, so the LOWEST score is evicted first — cheap to rebuild, bulky, and cold.
/// The default stands in for an UNKNOWN cost rather than acting as a floor.
/// Scoring an undeclared entry at zero would put it first in line, exactly backwards.
/// Flooring every entry would erase the difference between one that is genuinely cheap and one that simply never said.
/// `max(size, 4096)` floors the divisor at a page so a twelve-byte object cannot buy unbounded protection.
/// The decay is hyperbolic rather than exponential: it stays cheap in SQL, cannot underflow to zero, and two processes computing it always agree, because every input is on the row.

namespace bcache::impl
{
/// How much one slice is allowed to do.
/// Sized for latency rather than throughput: the actor drains its inbox between slices, so a get queued behind a collection waits for one slice rather than for the whole pass.
struct gc_budget
{
    i64 entries_per_batch = 64;
    i64 objects_per_batch = 64;
    i64 vacuum_pages = 256;
    i64 max_slices_per_pass = 64;
};

/// Runs reclamation against one connection.
/// Constructed on the actor thread over the actor's own connection, and never leaves it.
class cache_collector
{
public:
    cache_collector(babel::sqlite::database& db, gc_budget budget) : _db(db), _budget(budget) {}

    /// Deletes the entries the read path already found expired.
    /// Cheap, and it costs the expiry scan nothing.
    [[nodiscard]] cc::result<i64> remove_known_expired(cc::span<i64 const> entry_ids);

    /// One batch of entries whose expiry has passed, via the partial index.
    [[nodiscard]] cc::result<i64> remove_expired_batch(double now);

    /// The lowest-scoring entries, taken only until the overshoot is covered.
    ///
    /// **Target-aware rather than a fixed batch.** A blind batch would wipe a cache that holds fewer entries than the batch size, which is every small cache and
    /// every test — so candidates are accumulated in score order and the walk stops as soon as enough is scheduled to go.
    /// At least one is always taken, so a pass over a cache that is over its limit cannot fail to progress.
    ///
    /// An object several entries name is counted ONCE, since dropping the second entry naming it frees nothing more.
    [[nodiscard]] cc::result<i64> evict_batch(score_parameters const& params, i64 bytes_to_shed, i64 entries_to_shed);

    /// One batch of objects no entry references any more.
    ///
    /// **This is the only step that frees bytes.** Under deduplication, dropping an entry whose object another entry still names frees nothing at all — which is
    /// why an eviction phase must never conclude it is done because a batch reclaimed zero.
    [[nodiscard]] cc::result<gc_result> reclaim_orphan_batch();

    /// Returns a bounded slice of the freelist to the operating system.
    /// A no-op unless the file was created with auto_vacuum = INCREMENTAL, which cache_schema does before any table exists.
    void vacuum_slice();

    [[nodiscard]] gc_budget const& budget() const { return _budget; }

private:
    babel::sqlite::database& _db;
    gc_budget _budget;
};

/// The decay constant the score ages by, derived from the access quantum so the two never drift apart.
/// Twenty-four access epochs, floored at an hour — two hours at the default epoch of five minutes.
/// Pass the CLAMPED epoch, not a caller's raw one, or the two do drift apart for a sub-second setting.
[[nodiscard]] double half_life_for(double access_epoch_secs);
} // namespace bcache::impl
