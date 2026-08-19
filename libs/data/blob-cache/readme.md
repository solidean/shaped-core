# blob-cache

A persistent, multi-process cache for expensive derived bytes.
Namespace `bcache`. Depends on **clean-core**, and privately on **babel-serializer** for the SQLite engine behind it.

```cpp
#include <blob-cache/blob_cache.hh>

auto cache = bcache::blob_cache::create({.path = cache_path, .limits = {.max_total_bytes = 512 << 20}});

auto const key = bcache::cache_key{.space = bcache::cache_namespace("geometry.mesh_boolean"),
                                   .key = bcache::logical_key::create_from_hash(inputs_hash),
                                   .version = bcache::version(4)};

auto mesh = cache->acquire(key, [&] { return serialize(compute_boolean(a, b)); });
```

Headers are included by full path from `src/`: `#include <blob-cache/blob_cache.hh>`.

## What it is for

Results that are expensive to produce and cheap to recognize: geometry processing output, serialized acceleration structures, generated archives, downloaded artifacts, preprocessing results.
One database file, shared by every process on the machine that opens the same path.

## The one rule everything else follows from

**The cache is an optimization and nothing else.** Deleting the file at any moment, corrupting it, or building without a SQLite backend changes nothing a caller
computes — only how long it takes.

So no operation surfaces a storage failure as an error.
A failed read is a miss, a failed write is a `put_status` the caller may log and is free to ignore, and `acquire` returns the value it computed whether or not storing it worked.
The only failure that reaches a caller through `acquire` is its own compute's.

## Design at a glance

* **Entries are immutable and first-writer-wins.**
  `(namespace, key, version)` maps to one committed object until that entry is evicted, and a second `put` under a live key stores nothing.
  To replace a value, bump its `version`.
* **Objects are content-addressed**, so two entries whose bytes are identical name one object.
  That is also why evicting one of them frees nothing.
* **One actor owns the connection.** No other thread in the process touches the database, and no user code ever runs
  inside a transaction.
* **`acquire` singleflights the whole pipeline** — lookup, compute, store — not merely the compute.
* **Recency is approximate on purpose.** A hit costs no write: access times are deferred, deduplicated, quantized to
  an epoch and batched.
* **An incompatible file is discarded, never refused.** There is no migration code here and never will be.

Cross-process duplicate computation is accepted: two processes may compute the same key at once, and immutability plus content addressing make that benign.
[docs/design.md](docs/design.md) argues each of these, and says what it rules out.

## File organization

| Path | What is in it |
|---|---|
| `src/blob-cache/fwd.hh` | the API index — every public name with the line that says what it is |
| `src/blob-cache/keys.hh` | `cache_namespace`, `logical_key`, `version`, `cache_key`, `content_hash` |
| `src/blob-cache/blob_cache.hh` | the class, its options and its results |
| `src/blob-cache/impl/cache_schema.*` | the DDL, the stamping, and discard-and-recreate |
| `src/blob-cache/impl/cache_io.*` | the only place that talks to a database |
| `src/blob-cache/impl/cache_gc.*` | the eviction score, the candidate queries, the slices |
| `src/blob-cache/impl/cache_actor.*` | the message set and the connection owner |
| `src/blob-cache/impl/singleflight.*` | the in-process flight table |
| `src/blob-cache/impl/cache_core.hh` | the shared state an in-flight acquire reaches back into — why the handle holds a `shared_ptr` |
| `src/blob-cache/impl/cache_rows.hh` | the plain structs crossing the storage boundary; no sqlite type in any signature |

## Driving it

The cache hands work to `cc::async`, so something must drive that graph — the same contract every other async surface in the repo has.
Install the ambient scheduler at startup (`cc::install_default_async_scheduler`), which is what runs the returned handles.

Without threads, or with `cache_config::unthreaded`, there is no actor thread and `pump()` runs storage work on the calling thread.
A caller that never pumps sees only misses and dropped puts — degraded, never deadlocked.

## Building & testing

```bash
uv run dev.py test "blob-cache-test"
```

Never run the `blob-cache-test` binary directly.

## More

* [cheat-sheet.md](cheat-sheet.md) — the whole API, one symbol per line
* [docs/_index.md](docs/_index.md) — the design and its reasoning
* [../../../docs/coding-guidelines.md](../../../docs/coding-guidelines.md)
