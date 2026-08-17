# blob-cache design

Why the library is shaped the way it is, and what each decision rules out.
The API itself is the [cheat sheet](../cheat-sheet.md)'s; the headers carry the per-symbol contracts.

## The invariant everything hangs from

> Deleting all cache data can never affect correctness.

Every other decision here is downstream of that.
It is what makes a discarded schema acceptable rather than alarming, what makes a failed write a status rather than
an error, and what makes cross-process duplicate computation a cost rather than a bug.

The consequence a caller feels: `get` answers a miss, an expired entry and a storage failure identically.
The fallback is the same in all three cases, so telling them apart would only make every caller write the same swallow.
A reason is still available — `opened()` carries the one for a degraded open, and `on_storage_error` fires per failure — but for a log line, not for control flow.

## Three layers

```text
public API          get / put / acquire, and in-process singleflight
      |
      | async requests
      v
threaded actor      the sole owner of this process's SQLite connection
      |
      v
SQLite database     shared across processes, WAL-backed
```

The split means intra-process concurrency and cross-process concurrency are solved by different mechanisms and
never have to be reasoned about together: the actor serializes everything inside the process, SQLite serializes writers between them.

The connection is a member of the actor implementation, not of `blob_cache`.
There is no path from a cache handle to a `babel::sqlite::database`, only a mailbox — so exclusive ownership is a
fact of the structure rather than a convention somebody could break by adding one accessor.

**Mailbox order is load-bearing.** Messages dispatch in send order, so a `get` enqueued after a `put` sees that
put's writes, and no read ever interleaves with a write on this connection — which is what makes the blob handle a read streams through impossible for the actor to invalidate under itself.

## Identity: two hashes that must never be confused

```text
logical key   identifies a COMPUTATION, and exists before it runs
content hash  identifies the resulting BYTES, and cannot exist until it has
```

Singleflight keys on the first — it has to, since the second is not known until the compute is over.
Deduplication keys on the second.
They are separate types so that mixing them up does not compile.

A `logical_key` is stored verbatim and compared byte for byte, never hashed down.
Hashing it here would let a collision hand back another computation's blob, which is the one failure mode a cache may not have: it is allowed to miss, never to lie.
A caller that wants a fixed-size key hashes its own inputs and passes the digest — that collision is then its own to reason about, which is the right place for it.

## Immutability and first-writer-wins

`(namespace, key, version)` maps to one committed object until that entry is evicted.
A second `put` under a live key stores nothing and reports `already_present`.

This matters most where it is least expected.
Two processes may compute the same key at once and legitimately produce *different* bytes — two ZIP archives of the same files differing only in their embedded timestamps, say.
Either is a valid answer for the logical key.
What must not happen is the mapping flapping between them depending on who wrote last.

The schema makes this one statement rather than a race: `UNIQUE(namespace, key, version)` plus `INSERT OR IGNORE`.
The loser sees `changes() == 0`, leaves the refcount alone, and reports that it lost.
If it had already inserted an otherwise-unreferenced object, that object is an orphan and a later pass reclaims it.

`version` is the bulk invalidation mechanism: bumping it makes a whole generation of entries unreachable at once, and they age out as ordinary cold entries.
`invalidate` and `clear` drop entries one key and one namespace at a time.

## Singleflight covers the pipeline, not the compute

For each logical key there is at most one live `acquire` pipeline in a process, and it spans lookup, compute and store.
Covering only the compute would let two callers both look up, both observe a miss, and both enter computation — the exact duplication the mechanism exists to prevent.

The pipeline is one raw `cc::async` compute frame rather than a chain of continuations, because the node a caller holds must *be* the pipeline.
Only then does driving the result drive the work, which is what lets the cache impose no scheduler of its own beyond the one a caller's compute already needs.

Three properties of the table are each load-bearing:

* **The lock covers one map probe and one insert.** No database call, no mailbox enqueue and no compute ever runs
  under it — which is also why an `acquire` may be issued from a thread already holding a caller's own lock.
* **Operations are held weakly.** Once the last caller lets go, the operation is forgotten.
  An owning table would keep every blob it ever handed out alive, making a disk cache an unbounded in-memory one as a side effect.
  The cost is that a caller arriving just after the last one left re-reads from storage, which is a lookup, not a recompute.
* **Slots carry a generation.** An operation that finishes after a successor has already claimed the same key must
  not erase the successor's registration, or the next caller would start a second compute.

The terminal step releases the slot *before* resolving.
Resolving wakes dependents synchronously, and a dependent that immediately re-acquires the key must find the slot gone rather than joining an operation that is already over.

Cross-process singleflight is deliberately absent — the closing section says what that costs.

## Data model

```text
(namespace, key, version) -> entry -> object -> chunks
```

The separation is semantic, not incidental.
`created_at`, `accessed_at`, `expires_at` and `compute_secs` belong to the **entry**.
Two computations can reach identical bytes at wildly different cost and with completely different lifetimes, so none of those may become a property of the shared object.
The object carries only what is intrinsic to the bytes — the hash, the size, the chunk count — plus the refcount that says how many entries name it.

`expires_at` and `compute_secs` are **nullable, and NULL is load-bearing**.
Absent means "never expires" and "cost unknown", and both differ from a stored zero: a zero expiry is an entry that is already expired, and a zero cost is a caller saying this is free to rebuild.
Encoding either absence as a sentinel would collapse two answers a caller can legitimately give into one.
Both collapses point the wrong way, too: GC would read "never expires" as "expire it now", or "unknown" as "throw it away first".
That is why the public options are `cc::optional<double>` rather than doubles with a `<= 0` convention.

Objects are chunked at 1 MiB.
Chunking is not optional: SQLite caps a single value near a gigabyte.
1 MiB then puts per-row overhead under 0.01%, bounds the buffer a bind copies on the write path, and makes even a gigabyte object only 1024 rows.

`object_chunk` **must** be a rowid table: `babel::sqlite::blob_handle` addresses a cell by rowid and cannot reach a `WITHOUT ROWID` table at all.
Reads stream through that handle straight into the caller's buffer, so nothing is staged and no payload ever passes through a row view — which sidesteps the "valid only until the next step"
lifetime hazard entirely.

Chunk rowids are resolved per read rather than cached, because a `VACUUM` renumbers them under a file another
process holds, and a stale cache would hand back wrong bytes rather than merely be slow.

### Complete-object visibility

The object row, all of its chunks and the entry row are written in **one** transaction.
That is exactly why a committed entry can never reference a partially visible object, and why a crash mid-write
leaves the whole group absent rather than an entry pointing at a truncated object.

## Approximate recency

A cache hit must not cost a write.
An `UPDATE ... SET accessed_at` per hit would put every reader in contention with every other process's writer for a number nobody reads until a collection runs.

So access times are deferred, deduplicated per entry, quantized to an epoch, batched, and written under a guard:

```sql
UPDATE entries SET accessed_at = :epoch WHERE id = :id AND accessed_at < :epoch;
```

The guard does two jobs.
It never walks recency backwards past a value another process recorded — there is no cross-process ordering on this column at all.
And within one epoch it matches nothing, so SQLite dirties no page:
quantization plus the guard is what turns a hot key's hit stream into *zero* writes rather than merely batched ones.

A flush failure is swallowed and the buffer cleared regardless.
Losing a batch costs a slightly worse eviction decision; a buffer that grew on every failure would be the actual bug.

## Eviction

The database stores primitive signals and the policy that turns them into an order lives in one file, so it can be replaced wholesale without touching the schema.
**Do not build a public semantic around the current formula.**

Today it is cost-aware value density, aged:

```text
cost  = compute_secs > 0 ? compute_secs : default
score = (cost / max(size, 4096)) / (1 + (now - accessed_at) / half_life)
```

evaluated ascending, so the lowest score goes first — cheap to rebuild, bulky, and cold.

* Pure LRU is insufficient: two equally old gigabytes can differ by two orders of magnitude in what it costs to get
  them back.
* The divisor is size, so the question asked is "how expensive is this per byte of disk it occupies", not "how
  expensive is this" — 100 MB that took 20 s can be a better use of a gigabyte than 1 GB that took 100 s.
* The default stands in for an *unknown* cost rather than acting as a floor.
  Scoring an undeclared entry at zero would put it first in line, exactly backwards; flooring every entry would erase the difference between one that is genuinely cheap and one that simply never said.
* `max(size, 4096)` floors the divisor at a page, so a twelve-byte object that took twenty minutes cannot buy
  effectively unbounded protection.
* The decay is hyperbolic rather than exponential: cheap in SQL, cannot underflow to zero, and two processes
  computing it always agree because every input is on the row.

### The pass, and the deduplication trap

A pass runs in slices, one transaction each: expiry, then score-ranked eviction while over target, then orphan reclamation, then a bounded `incremental_vacuum`.
The actor re-drains its inbox between slices, so a `get` queued behind a large collection waits for one slice rather than for the pass.

Expired entries are always taken first, whatever they score.
That is what lets an application cache a big short-lived artifact aggressively without it ever crowding out unrelated durable content.

**Reclamation is the only step that frees bytes.** Deleting an entry whose object another entry still names frees nothing at all.
Two consequences follow, and both are easy to get wrong:

* An eviction phase must not stop because a batch reclaimed zero — on a fully shared cache every batch would.
* An eviction phase must not *continue* on the same stale total either.
  `stored_bytes` only falls during reclamation, so a second batch decided against the pre-eviction number would evict again for space the first batch already scheduled to free.
  On a small cache that throws away the single most valuable entry.
  So each eviction batch hands off to reclamation, and reclamation comes back only if it freed something and the cache is still over.

Candidates are accumulated in score order only until the overshoot is covered, counting each object once.
A blind fixed-size batch would wipe any cache holding fewer entries than the batch size.

Accounting is re-read from the file at open, and at both ends of every pass.
Another process's puts and evictions are invisible to an incremental counter, and one aggregate over a small table once per interval costs nothing next to being wrong about the ceiling.
Between passes the counter is maintained incrementally so a put can cheaply notice it crossed the limit.

The limits count decoded object bytes, not pages, indexes, the freelist or the WAL — the real file runs roughly 1.1-1.3x larger, and `cache_stats::file_bytes` is the number for the disk.

## Schema handling: discard, never refuse

An incompatible file is dropped and recreated.
A `user_version` mismatch in either direction, a foreign `application_id`, or a table missing a column this build addresses all trigger it.
An extra column is a newer build's and is kept, because nothing here addresses it — discarding on one would make two builds sharing a machine
wipe the cache from each other on alternate runs.

`vdoc::file` rightly refuses a format version from the future, because guessing would risk somebody's document.
A cache holds nothing anyone would miss, so refusing would only strand a caller on a stale file forever.
There is no migration code here and there never will be.

The discard is `DROP TABLE` plus recreate, in one transaction on the open connection: other processes hold the same
path and see the new schema at their next statement rather than a file that vanished under them.
Unlinking is the fallback for a file whose header cannot be read at all, and it removes the `-wal` and `-shm` siblings too — a stale
WAL beside a fresh file is its own corruption, and `cc::remove_file` knows nothing about siblings.

One further ordering trap: `PRAGMA auto_vacuum = INCREMENTAL` must run **before the first table exists**.
It cannot be changed later without a full `VACUUM`, so a statement one line too late means the file's freed pages never return to the operating system.

## Degradation

An absent SQLite backend, a missing directory, a read-only file, a full disk and mid-run corruption all land on one flag, one code path and one message.
Every later request then answers as an empty cache: `get` misses, `put` is `unavailable`, a collection reports zeros.
`acquire` is unaffected in shape — it still singleflights, since that is pure in-process machinery, and still returns what it computed.

Availability is a runtime probe (`is_storage_available()`), never a macro.
A declaration that disappeared would make a caller's code depend on how the build was configured; a probe that returns false does not.

## Threading

There is no `#if` anywhere in this library.
Both dimensions — threads and storage — are runtime facts.

Without threads, or with `cache_config::unthreaded`, there is no actor thread and `pump()` runs storage work on the calling thread.
It returns false in a threaded build, so pumping unconditionally is correct everywhere.
A caller that never pumps sees only misses and dropped puts: degraded, never deadlocked, which is the property that makes the rule safe rather than a trap.

## What is deliberately not here

* **Cross-process singleflight** — two processes may compute one key at once.
  Immutability and content addressing make that benign, and a lease is a lot of machinery for duplicate work nobody has measured.
* **Compression** — the caller knows what its bytes are; the cache does not.
* **Streaming public APIs** — chunking is an internal persistence detail today, and the seam for streaming later.
* **A public delete beyond `invalidate` / `clear`** — `version` covers most invalidation, and expiry and collection
  cover the rest.
* **Exact accounting after every mutation** — approximate is the point.
