# blob-cache cheat sheet

A persistent, multi-process cache for expensive derived bytes, keyed on `(namespace, key, version)` and deduplicated by content hash.
Namespace `bcache`; headers included by full path from `src/`.

> **The cache is an optimization and nothing else.** A storage failure is a miss or a dropped write, never an error.
> [readme.md](readme.md) is the front door; [docs/design.md](docs/design.md) is the reasoning.

How to read this: each block leads with the include; one symbol per line with a trailing comment giving the return type or the intuition.
Format conventions live in [docs/guides/cheat-sheets.md](../../../docs/guides/cheat-sheets.md).

---

## Identity

```cpp
#include <blob-cache/keys.hh>

bcache::cache_namespace ns("geometry.mesh_boolean");  // the caller's partition; stored verbatim on every entry row
bcache::version(4);                                   // enum class : i32 — bump to invalidate a whole generation

bcache::logical_key::create_from_string(sv);          // opaque bytes, compared BYTE FOR BYTE and never hashed down
bcache::logical_key::create_from_bytes(span);
bcache::logical_key::create_from_hash(h256);          // the composite-key path: hash your inputs, pass the digest

bcache::cache_key{.space = ns, .key = k, .version = bcache::version(4)};   // ==, hash — usable as a map key

bcache::content_hash::create(bytes);                  // BLAKE3-256 over the STORED bytes; ==, hash
h.value;                                              // cc::hash256; to_bytes/from_bytes is the durable 32-byte form
```

`logical_key` identifies a **computation** and exists before it runs; `content_hash` identifies the resulting **bytes** and cannot exist until it has.
Singleflight keys on the first, deduplication on the second.

## The cache

```cpp
#include <blob-cache/blob_cache.hh>

using blob = cc::pinned_data<cc::byte const>;   // bcache::blob — shared, never deep-copied, OUTLIVES the cache

bcache::blob_cache::create(config);             // -> cc::unique_ptr<blob_cache>; NEVER fails, touches no disk here
bcache::blob_cache::create_disabled();          // caching off, with no branching at any call site
bcache::blob_cache::is_storage_available();     // -> bool; runtime probe, never a macro — what a test SKIPs on
cache->opened();                                // -> shared_async<cc::unit>; errors carry why storage is unavailable

cache->get(key);                                // -> shared_async<cc::optional<cache_hit>>; miss/expired/failed alike
cache->put(key, data, opts = {});               // -> shared_async<put_result>; fire-and-forget is the intended use
cache->acquire(key, compute, opts = {});        // -> shared_async<blob>; compute returns shared_async<blob>...
cache->acquire(key, [] { return blob; });       // ...or a plain blob, wrapped in make_async_lazy for you

cache->invalidate(key);                         // -> shared_async<bool>; the object survives until a pass finds it orphaned
cache->clear(ns);                               // -> shared_async<i64> entries dropped

cache->set_limits(limits);  cache->get_limits();
cache->collect_garbage();                       // -> shared_async<gc_result>; a whole pass (still slice-bounded), for tests
cache->flush();                                 // -> shared_async<cc::unit>; write buffered access times out
cache->get_stats();                             // -> cache_stats; cheap, never touches the database

cache->pump();                                  // -> bool; runs storage work where there is no actor thread
cache->close();                                 // flush, drain, join; idempotent, and the destructor calls it
cache->is_closed();                             // -> bool; a closed cache misses and drops rather than queueing
```

## Options and results

```cpp
bcache::cache_config{
    .path = p,                            // ITS DIRECTORY MUST ALREADY EXIST — a missing one opens degraded
    .limits = {...},
    .access_epoch_secs = 300,             // recency is rounded down to this before being written
    .access_flush_threshold = 256,
    .access_flush_interval_secs = 5,
    .gc_interval_secs = 60,
    .default_compute_time_secs = 0.01,    // what an entry with NO recorded cost is charged when scored
    .verify_on_read = false,              // re-BLAKE3 every hit; a full crypto pass per read, so off by default
    .wall_clock = fn, .steady_clock = fn, // default to clean-core's; must be callable from any thread
    .on_storage_error = fn,               // one line per failure, so degradation is never silent
    .unthreaded = false};                 // drive via pump() instead of an actor thread

bcache::cache_limits{
    .max_total_bytes = 1 << 30,           // decoded OBJECT bytes — not pages, indexes, freelist or WAL
    .target_total_bytes = 0,              // what a pass drives down to; 0 means 90% of max (the hysteresis)
    .max_entries = 0,                     // 0 = unlimited
    .max_object_bytes = 256 << 20};       // over this is never stored, and never takes a write lock

bcache::put_options{.ttl_secs = 0,             // <= 0 never expires
                    .compute_time_secs = 0,    // <= 0 means UNKNOWN, and scoring charges the default
                    .metadata = {}};           // handed back with a hit; keep it SMALL, GC scans this row
bcache::acquire_options{.put = {...}, .bypass_lookup = false};

bcache::put_status::stored                 // the entry and its object are now in the file
bcache::put_status::deduplicated           // new entry, object already there under the same hash — no bytes written
bcache::put_status::already_present        // somebody else got there first, and THEIR value is what get() returns
bcache::put_status::rejected_too_large     // over max_object_bytes
bcache::put_status::unavailable            // storage absent or degraded, or the write failed
put_result{.status, .hash};  r.is_present();   // -> bool: an entry exists, whoever wrote it

cache_hit{.data, .hash, .metadata};
gc_result{.entries_expired, .entries_evicted, .objects_reclaimed, .bytes_reclaimed, .is_incomplete};
cache_stats{.hits, .misses, .expired_as_miss, .puts_stored, .puts_deduplicated, .puts_lost_race, .put_failures,
            .computes_started, .singleflight_joins, .access_rows_written, .entries_evicted, .bytes_reclaimed,
            .stored_bytes, .entry_count, .file_bytes, .is_backed_by_storage};
```

## Gotchas

Only what the signatures above cannot tell you.

- **A miss, an expired entry and a storage failure are one answer.** The caller's fallback is identical in every
  case, so distinguishing them would only make every caller write the same swallow.
  `opened()` and `on_storage_error` are where a reason is available, for a log line.
- **Entries are immutable and first-writer-wins.** A second `put` under a live key stores nothing, even when its bytes differ.
  Bump `version` to replace a value.
- **`acquire` singleflights the WHOLE pipeline**, lookup included — so a joiner never runs the compute, and never looks up either.
- **A cold `acquire` runs only when something drives it.** It is scheduled here if a worker scope is active or a
  default pool is installed, and left cold otherwise for the caller to drive — the same contract as every other `cc::async` surface.
- **The flight table holds operations WEAKLY.** Once the last caller lets go, the operation is forgotten, and a
  later `acquire` re-reads from storage.
  That extra lookup is the price of the cache not becoming a second, unbounded in-memory cache of every blob it ever handed out.
- **A hit outlives the cache.** The blob is a pin over its own buffer, owned by whoever holds it and by nobody else.
- **`max_total_bytes` counts decoded object bytes**, not the file.
  Pages, indexes, the freelist and the WAL push the real file to roughly 1.1-1.3x; `cache_stats::file_bytes` is the number for the disk.
- **Evicting an entry can free zero bytes.** Under deduplication the object survives until its last entry is gone,
  which is why a collection alternates eviction with reclamation rather than counting evicted entries as space.
- **An unrecorded `compute_time_secs` means unknown, not free.** It is charged `default_compute_time_secs` — but
  that is a substitute, not a floor, so an honestly-measured small cost stays small.
- **`metadata` sits inline in the row every candidate scan reads.** Megabytes there wreck eviction.
- **An incompatible file is discarded and recreated, silently.** A `user_version` mismatch in EITHER direction, a
  foreign `application_id`, or a missing column all wipe it.
  An extra column is a newer build's and is kept.
- **`cache_config::path`'s directory must already exist.** clean-core has no directory creation; a missing one is
  not an error, it just opens degraded.
- **Without threads, whoever would have blocked must `pump()`.** A caller that never pumps sees only misses and
  dropped puts — degraded, never deadlocked.
  `pump()` is a no-op returning false in a threaded build, so calling it unconditionally is correct everywhere.
