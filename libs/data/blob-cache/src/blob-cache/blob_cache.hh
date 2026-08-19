#pragma once

#include <blob-cache/fwd.hh>
#include <blob-cache/keys.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>

#include <memory>      // std::shared_ptr — impl::cache_core outlives this handle, see the member
#include <type_traits> // std::is_invocable_r_v — what keeps the two acquire() overloads disjoint

/// The public surface of blob-cache.
/// [docs/design.md](../../docs/design.md) is the reasoning; this header is the contract.

// ---- options and results -------------------------------------------------------------------------

/// What one put actually did.
enum class bcache::put_status : bcache::u8
{
    stored,       ///< the entry and its object are now in the file
    deduplicated, ///< the entry is new, but the object was already there under the same hash, so no bytes were written
    already_present,    ///< another writer got there first, and ITS value is what get() returns — first writer wins
    rejected_too_large, ///< over cache_limits::max_object_bytes; nothing was written and no write lock was taken
    unavailable         ///< storage is absent or degraded, or the write failed; nothing was stored
};

struct bcache::put_result
{
    put_status status = put_status::unavailable;
    content_hash hash;

    /// Whether an entry for this key now exists, whoever wrote it.
    [[nodiscard]] bool is_present() const
    {
        return status == put_status::stored || status == put_status::deduplicated
            || status == put_status::already_present;
    }
};

struct bcache::cache_hit
{
    blob data;
    content_hash hash;
    cc::vector<cc::byte> metadata;
};

struct bcache::put_options
{
    /// Seconds from now until this entry stops being returned.
    /// Absent means it never expires, which is a different statement from a TTL of zero — that one is already expired.
    cc::optional<double> ttl_secs;

    /// What producing these bytes cost, in seconds — the recompute price GC weighs against the space they occupy.
    /// Absent means UNKNOWN, and GC then charges cache_config::default_compute_time_secs rather than reading it as free.
    /// acquire() fills this in from its own measurement when the caller left it absent.
    cc::optional<double> compute_time_secs;

    /// Opaque bytes handed back with a hit.
    /// Must stay SMALL: they sit inline in the row every GC candidate scan reads, so megabytes here wreck eviction.
    cc::vector<cc::byte> metadata;
};

struct bcache::acquire_options
{
    /// Applied to the entry this acquire may write.
    put_options put;

    /// Skip the lookup, compute, then put.
    /// Entries are immutable and first-writer-wins, so this REFRESHES NOTHING — it exists to measure the cold path.
    bool bypass_lookup = false;
};

/// The ceilings a garbage-collection pass enforces.
///
/// All of them are approximate, and deliberately so: they count decoded object bytes, not pages, indexes, the freelist
/// or the WAL, so the file on disk runs meaningfully larger — call it 1.1-1.3x.
/// cache_stats::file_bytes is the number to read when the disk is what matters rather than the policy.
struct bcache::cache_limits
{
    i64 max_total_bytes = i64(50) << 30;

    /// What a pass drives down to once it starts — the hysteresis that keeps GC off the put path.
    /// 0, or anything above max_total_bytes, is silently taken as 90% of it rather than rejected.
    i64 target_total_bytes = 0;

    /// 0 means unlimited.
    i64 max_entries = 0;

    /// An object larger than this is never stored: holding it would evict most of the cache to make room.
    /// 0 means no limit.
    i64 max_object_bytes = i64(5) << 30;
};

struct bcache::cache_config
{
    /// The SQLite file.
    ///
    /// ITS DIRECTORY MUST ALREADY EXIST — clean-core has no directory creation, so the caller makes it.
    /// A missing directory is not an error: the cache opens degraded and every get misses.
    cc::string path;

    cache_limits limits;

    /// Access times are rounded down to this many seconds before being written, so a hot entry's row is touched once per bucket instead of once per hit.
    /// Clamped to at least one second, and the eviction score's decay is derived from the clamped value.
    double access_epoch_secs = 300;

    /// Clamped to at least 1.
    i64 access_flush_threshold = 256;
    double access_flush_interval_secs = 5;

    double gc_interval_secs = 60;

    /// What an entry with no recorded compute_time_secs is charged during eviction scoring; a negative value is clamped to 0.
    /// Unknown is not free: scoring such an entry at zero would make it the FIRST thing evicted, which is backwards.
    double default_compute_time_secs = 0.01;

    /// Recompute the BLAKE3 of every hit and answer a mismatch as a miss.
    /// Off by default: it costs a full cryptographic pass per hit, an order of magnitude over the read itself.
    bool verify_on_read = false;

    /// Both default to clean-core's clocks when unset.
    /// MUST be safe to call from any thread — the actor thread and the calling thread both read them.
    cc::unique_function<double()> wall_clock;
    cc::unique_function<double()> steady_clock;

    /// Called on the actor thread whenever a storage operation fails, for one log line.
    /// The cache degrades to a miss either way; this exists so the failure is not silent.
    cc::unique_function<void(cc::string_view)> on_storage_error;

    /// Run storage on whoever blocks, through the cc::register_thread_pump registry, instead of on an actor thread.
    /// A build with CC_HAS_THREADS == 0 behaves this way whatever this says.
    bool unthreaded = false;
};

struct bcache::gc_result
{
    i64 entries_expired = 0;
    i64 entries_evicted = 0;
    i64 objects_reclaimed = 0;
    i64 bytes_reclaimed = 0;

    /// True where the pass hit its slice budget with work left; the next interval picks it up.
    bool is_incomplete = false;
};

struct bcache::cache_stats
{
    i64 hits = 0;
    i64 misses = 0;
    i64 expired_as_miss = 0;
    i64 puts_stored = 0;
    i64 puts_deduplicated = 0;
    i64 puts_lost_race = 0;
    i64 put_failures = 0;
    i64 computes_started = 0;
    i64 singleflight_joins = 0;
    i64 access_rows_written = 0;
    i64 entries_evicted = 0;
    i64 bytes_reclaimed = 0;

    /// As of the last GC pass, not as of now.
    i64 stored_bytes = 0;
    i64 entry_count = 0;

    /// The database file itself, page_count * page_size — indexes, freelist and all.
    /// Also as of the last pass.
    i64 file_bytes = 0;

    /// False when SQLite is absent, the open failed, or a failure degraded the connection.
    bool is_backed_by_storage = false;
};

// ---- the cache -----------------------------------------------------------------------------------

/// A persistent, multi-process blob cache: (namespace, key, version) -> content-addressed bytes.
///
/// **The cache is an optimization and nothing else.** Deleting the file, corrupting it, or building without a SQLite backend changes nothing a caller computes — it only
/// makes them compute more.
/// So no operation here surfaces a storage failure as an error: a failed read is a miss, and a failed write is a put_status the caller may log and is free to ignore.
///
/// Entries are IMMUTABLE and FIRST-WRITER-WINS.
/// A second put under a key that exists stores nothing and reports `already_present`; there is no update path, because
/// a mutable entry shared across processes has no ordering anyone could reason about.
/// To replace a value, bump its `version`.
///
///     auto cache = bcache::blob_cache::create({.path = cache_path, .limits = {.max_total_bytes = 512 << 20}});
///     auto shader = cache->acquire(key, [&] { return compile_shader(source); }, {.put = {.ttl_secs = 7 * 24 * 3600}});
///
/// Callable from any thread: the singleflight table is internally locked, and everything past it is serialized by the actor that owns the connection.
class bcache::blob_cache
{
    // creation
public:
    /// Opens or creates the cache at config.path, and hands back a usable handle IMMEDIATELY, having touched no disk.
    ///
    /// Never fails.
    /// An absent backend, a missing directory, an unwritable or damaged file all leave a handle that misses on every get and drops every put — which is the whole point of the type.
    [[nodiscard]] static cc::unique_ptr<blob_cache> create(cache_config config);

    /// A cache with no storage at all: every get misses, every put is dropped, and acquire still singleflights.
    /// What a caller uses to turn caching off without branching on it anywhere.
    [[nodiscard]] static cc::unique_ptr<blob_cache> create_disabled();

    /// Whether a SQLite backend was linked in at all.
    /// A runtime probe, never a macro: false makes create() report the absence rather than making a declaration disappear, and it is what a test SKIPs on.
    [[nodiscard]] static bool is_storage_available();

    /// Ready once the open finished; its error channel carries why storage is unavailable, for one log line.
    /// Waiting on it is optional — the cache answers correctly both before and after it resolves.
    [[nodiscard]] cc::shared_async<cc::unit> opened() const;

    // reading and writing
public:
    /// The cached bytes for `key`, or an empty optional.
    ///
    /// A miss, an expired entry and a storage failure are all the same answer, on purpose: the caller's fallback is
    /// identical in every case, so distinguishing them would only make every caller write the same swallow.
    /// The error channel carries cancellation at shutdown and nothing else.
    [[nodiscard]] cc::shared_async<cc::optional<cache_hit>> get(cache_key key);

    /// Stores `data` under `key` if nothing is there yet; see put_status for what actually happened.
    /// Fire-and-forget is the intended use — the returned async exists so tests and close() can wait on it.
    [[nodiscard]] cc::shared_async<put_result> put(cache_key key, blob data, put_options options = {});

    /// The cached bytes for `key`, or `compute()`'s, stored on the way back.
    ///
    /// **Singleflight.** Concurrent acquires of the same key in this process share one operation and one `compute`.
    /// `compute` runs only on a miss, and only for whoever won the race; joiners never see it at all.
    /// The cover is the WHOLE pipeline — lookup, compute, store — not just the compute, or two callers would both look up, both miss, and both compute.
    ///
    /// **A cache failure is never visible.** A failed lookup runs compute; a failed put still returns the computed value, so a broken cache is slow and nothing else.
    /// The result errors only where `compute`'s own async errors, or at shutdown.
    [[nodiscard]] cc::shared_async<blob> acquire(cache_key const& key,
                                                 cc::unique_function<cc::shared_async<blob>()> compute,
                                                 acquire_options options = {});

    /// acquire() for a plain blocking producer.
    /// `compute` is wrapped in cc::make_async_lazy, so it runs on the async pool rather than on the calling thread — and, with no pool installed, wherever the caller drives the graph.
    template <class F>
        requires(std::is_invocable_r_v<blob, F&>)
    [[nodiscard]] cc::shared_async<blob> acquire(cache_key const& key, F compute, acquire_options options = {})
    {
        // compute is moved once more, into the lazy node's own frame: the wrapper is destroyed as soon as it returns,
        // so a frame that captured it by reference would run against a dead callable.
        return this->acquire(
            key,
            cc::unique_function<cc::shared_async<blob>()>(
                [compute = cc::move(compute)]() mutable
                { return cc::make_async_lazy<blob>([f = cc::move(compute)]() mutable { return f(); }); }),
            cc::move(options));
    }

    // entries
public:
    /// Drops the logical entry for `key`; the object it named survives until a pass finds it orphaned.
    /// Resolves false where there was nothing to drop.
    [[nodiscard]] cc::shared_async<bool> invalidate(cache_key key);

    /// Drops every entry in `space`, resolving with how many.
    /// Objects follow at the next pass.
    [[nodiscard]] cc::shared_async<i64> clear(cache_namespace space);

    // limits and maintenance
public:
    /// Applied from the next pass; lowering max_total_bytes schedules one immediately.
    void set_limits(cache_limits limits);
    [[nodiscard]] cache_limits get_limits() const;

    /// Runs a pass now — expiry, then eviction to the target, then orphan reclamation — rather than one slice.
    /// For tests and for an explicit "free disk now"; normal operation never calls it.
    /// Still bounded: a cache far enough over its limit comes back with gc_result::is_incomplete rather than looping.
    [[nodiscard]] cc::shared_async<gc_result> collect_garbage();

    /// Writes buffered access times out now.
    /// Advisory: tests wait on it, nothing else needs to.
    [[nodiscard]] cc::shared_async<cc::unit> flush();

    /// Counters since create(). Cheap, and never touches the database.
    [[nodiscard]] cache_stats get_stats() const;

    // shutdown
public:
    /// Flushes buffered access times, drains accepted writes, rejects new ones, and joins the actor.
    /// Idempotent, and the destructor calls it.
    ///
    /// Acquires still in flight are CANCELLED rather than left hanging: their compute may never have been driven, and a joiner waiting on work nobody will do would wait forever.
    void close();

    [[nodiscard]] bool is_closed() const;

    /// Constructs the DISABLED cache — every get misses and every put is dropped.
    /// create() and create_disabled() are how one is normally made; this exists so a blob_cache can be a plain member that a later create() assignment is not needed to make valid.
    blob_cache();
    ~blob_cache();

    blob_cache(blob_cache const&) = delete;
    blob_cache(blob_cache&&) = delete;
    blob_cache& operator=(blob_cache const&) = delete;
    blob_cache& operator=(blob_cache&&) = delete;

private:
    /// SHARED, not owned outright: an acquire's continuations reach back into this after the caller has let go of its
    /// handle, and a continuation pointing at a destroyed table is the failure this prevents.
    std::shared_ptr<impl::cache_core> _core;
};
