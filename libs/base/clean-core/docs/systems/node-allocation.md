# node allocation

The node allocator ([node_allocation.hh](../../src/clean-core/memory/node_allocation.hh),
[node_allocation.cc](../../src/clean-core/memory/node_allocation.cc)) is a slab allocator for
**small, single objects** — list/map/set nodes, `unique_ptr` payloads, `unique_function` storage.
The goal is a **maximally cheap thread-local fast path** for these; it also gives two things the
general resource can't cheaply offer: an 8-byte owning handle (the class is derived from the *type*,
so nothing but a pointer is stored) and **stateless cross-thread deallocation** (no reference to the
allocator needed — the freeing thread finds everything from the pointer).

It is the **less mature** of clean-core's two memory systems, but the fast path and the full slab
lifecycle are now resolved. The fast path is **two compile-time frontends** selected by `CC_HAS_THREADS`
(see [Model](#model)); the refill path retains previous slabs in a ring instead of orphaning them, a
thread's slabs are handed off and adopted across thread exit rather than leaked, and surplus fully-free
slabs are trimmed back to the backing resource (see [Slab lifecycle](#slab-lifecycle-across-threads)).
The design target is a **fixed set of long-lived threads**: that workload pays nothing for any of the
lifecycle machinery. A thread-churning server stays correct and bounded but is second class — it pays a
rare per-class lock on thread exit and on slab adoption.

## Model

Allocations are bucketed into power-of-two **size classes**: class index `i` ⇒ size `2^i` bytes.
The class for a type is `bit_width(max(sizeof(T), alignof(T)) - 1)`, so both size and alignment are
satisfied. Classes `0..8` (1 B … 256 B) are the fast **small path**; anything larger takes a separate
header-backed **large path**.

Each small class is served from **slabs**: a `64 * class_size` block, aligned to its own size, so the
slab base is recovered from any interior pointer by `ptr & ~(slab_size - 1)` — a single AND, no
metadata lookup. Data starts at slab offset 0; slots that overlap the slab's metadata are permanently
blocked by a per-class **consteval seed mask** (`node_compute_seed_local_freemap`). The metadata layout
is **frontend-dependent** (`CC_HAS_THREADS`):

- **Threaded** (native, wasm+pthreads): `local` free bitmap `@0` (u64), `owner_id` `@8` (u32),
  next-slab pointer `@16`; a second **`remote`** free bitmap on the 2nd cache line `@64` (for classes
  whose slab spans two lines, i.e. ≥ 2 B) or `@24` for the single-line 1 B class.
- **Single-threaded** (wasm without `-pthread`): `local` free bitmap `@0`, next-slab pointer `@8`. No
  owner/remote — the second cache line is not touched.

The metadata consumes whole slots, so usable capacity depends on class size and frontend:

| class size | usable (threaded) | usable (single) |
|---:|---:|---:|
| 1 B | 36 | 48 |
| 2 B | 50 | 62 |
| 8 B | 60 | 62 |
| ≥ 16 B | ~62 | 63 |

The **fast path is two frontends behind an unchanged API**, selected at compile time:

- **Threaded (`step2_tls_diff`).** The owning thread allocates and frees **non-atomically** into `local`
  (find a free bit via `count_trailing_zeroes`, clear it; free sets it). A *genuinely remote* thread
  frees with a single `atomic_or` into `remote` (a separate cache line, so it never invalidates the
  owner's hot `local` line). When `local` empties, the owner drains `remote` into it via one atomic
  `exchange` on the cold path. The owner is identified by a process-unique, never-recycled `owner_id`
  stamped into the slab at hydrate and compared against a thread-local token on free.
- **Single-threaded.** Plain non-atomic `and`/`or` on the one `local` bitmap — no owner, no remote, no
  atomics at all.

Because the class index comes from the pointer + `sizeof`/`alignof`, **free needs no allocator or
resource** — this is what makes cross-thread free stateless.

Handles: `node_allocation<T>` (typed, move-only), `any_node_allocation` (type-erased pointer + deleter
+ class index, for wrappers with no natural base class), and `poly_node_allocation<T, NodeTraits>` (the
class index is produced dynamically by user traits, enabling polymorphism).

The large path (> 256 B) allocates from `default_memory_resource` (mimalloc) with a 24-byte header
`[size][alignment][resource*]`; free reads the header back. So a large node is *just mimalloc plus a
header* — not the slab machinery at all.

## Slab lifecycle across threads

A slab moves through four states; the hot path only ever sees **owned**, so none of this touches the
fast-path codegen.

```text
unhydrated ──refill──▶ owned(T) ──thread exit, has live nodes──▶ orphaned ──refill on any thread──▶ owned(M)
                          │                                          │
              thread exit, fully free /                     (never adopted, but fully
                    trim, fully free                          free) waits for a taker
                          │
                          ▼
                   freed to backing
```

- **owned(T).** The hydrating thread `T` owns the slab (its process-unique, never-recycled `owner_id`
  is stamped in). `T` allocates and frees plain-local; every other thread routes its frees to `remote`.
- **Abandonment (thread exit).** When a thread's `thread_local` allocator is destroyed, it reclaims its
  ring **through a resource hook** (`node_memory_resource::reclaim_slabs`) — the system resource
  implements it; a resource without the hook keeps the old leak-on-exit. Per class, under a per-class
  lock: each slab's `remote` is drained into `local`; **fully-free** slabs (no live nodes) go back to
  the backing resource; the rest are pushed onto a per-class **orphan bin**. Because `owner_id` is never
  recycled and the owner only stops owning by *dying*, every subsequent free into an orphaned slab is
  guaranteed to take the `remote` path — no plain-local writer can race the future adopter.
- **Adoption (refill).** Before mallocing a fresh slab, refill pops one orphan of the class if present,
  re-stamps `owner_id` to the adopting thread, drains its accumulated `remote` frees, and splices it into
  the ring. `owner_id` is only ever *stamped* atomically (an `atomic_ref` store on the cold refill path);
  the hot free-path read stays a plain `u32` load.
- **Trim.** On the cold allocation path only (never on free), a per-class counter gates a rare O(ring)
  sweep that returns surplus fully-free slabs to the backing resource, keeping one fully-free spare so a
  steady working set never re-mallocs. A stable set of live nodes never produces a fully-free slab, so a
  long-lived thread never actually trims.

The per-class orphan bins are `constinit` spinlocks (not `std::mutex`) so they are never torn down —
the main thread's allocator can reclaim safely even during static destruction. Ownership is only ever
transferred at thread exit; there is deliberately **no mid-life slab migration** between live threads
(that would force an atomic `owner_id` and cost the hot path).

## Gotchas

- **`owner_id` is a `u32` and is never recycled.** Abandonment reclaims *slabs*, not *ids*, so the
  counter stays monotonic (this is what keeps a live id from ever being reused and a free from being
  miscategorized). The id space is a `u32`; an assert fires past ~4 B threads-*ever*-created. A fixed
  thread pool never wraps; a server that spawns billions of threads over its lifetime would — accepted,
  and loud when hit (see [philosophy.md](../../../../../docs/philosophy.md), "fail loud").

- **Cross-thread frees into an orphaned-but-never-adopted slab accumulate in `remote`.** If a thread
  hands out nodes and exits and *no* later thread ever allocates that class again, the orphaned slab sits
  in its bin holding the still-live nodes; frees keep landing in `remote` and are only reclaimed once the
  slab is adopted. Bounded (one bin per class), not a leak in the grow-unbounded sense, but worth knowing.

- **Over-aligned large nodes are unsupported.** `system_allocate_node_bytes_large` asserts
  `alignment == 8` ([node_allocation.cc:43](../../src/clean-core/memory/node_allocation.cc#L43)); larger
  alignments are a `TODO`.

- **Alloc-heavy workloads re-walk the ring on each exhaustion**
  ([node_allocation.cc](../../src/clean-core/memory/node_allocation.cc)) — no bookkeeping caches the
  last-known-full point, so exhaustion is O(ring) each time. A `TODO`.

- **Allocation is single-threaded per allocator.** A `node_allocator` is a thread-local cache and must
  not allocate from two threads; only *free* is concurrency-safe. A `node_memory_resource` that wants to
  serve many threads does so by handing out different allocators (e.g. via TLS), which the default does.

- **Slab metadata layout is a whole-build switch.** The frontend (and thus the slab layout) is chosen by
  `CC_HAS_THREADS` at compile time, so slabs never cross frontends. Do not mix objects built with and
  without threads in one binary.

Done on this branch: the **local/remote bitmap split** (the two `lock`-prefixed RMWs on the fast path are
gone — the owner is fully non-atomic; only genuine cross-thread frees pay an atomic), **non-leaking
refill** (previous slabs are retained in the ring), and the full **slab lifecycle across threads** —
thread-exit abandonment, cross-thread adoption, and trim (see above). Still open:

- **Cheaper ring re-walk** — cache the last-known-full point instead of an O(ring) scan per exhaustion.
- **Over-aligned (> 8 B) large nodes** — `system_allocate_node_bytes_large` still asserts `alignment == 8`.
- **Co-located refcounts** — the header notes a possible future `shared_ptr`-like variant storing the
  count next to the node.
- **`any_node_allocation` reserved bytes** — it has 7 padding bytes earmarked for future use.

## Throughput — where it actually stands

**Speed matters, but it is not the reason this allocator exists** — the 8-byte handle and the stateless
wait-free free are (see Model). On throughput it is already competitive-to-ahead for the small classes it
targets, which is the *opposite* of what an earlier draft of this section claimed.

> The table below was measured on the **prior two-lock fast path** (before the local/remote split landed on
> this branch). It is retained as the mimalloc baseline and the pre-split reference. The split removes both
> `lock`-prefixed RMWs on the owner path (confirmed via `dev.py assembly show`), so current small-class
> numbers are **higher** than shown — the design benchmark projects ~2.3x the old node throughput at 16 B.
> Re-running the comparison on the shipped split is a follow-up; see
> [`bench-node-design (fast-path variants)`](../../tests/benchmarks/node-allocation-design-benchmark.cc).

From the [handle & node comparison benchmark](../../tests/benchmarks/allocation-benchmark.cc)
(`bench-alloc (handle & node comparison)`, manual; 32 live nodes to stay within one slab), cross-checked
against the batch/interleaved
[`bench-alloc (steady-state small batch)`](../../tests/benchmarks/allocation-benchmark.cc). Metric is
**millions of alloc+free cycles per second — higher is better** (one cycle = one free + one allocate).
Release build (`release-clang`), **Ryzen 9 7950X3D**; 3-run medians, run-to-run spread < 3%:

| size (B) | node (M cyc/s) | mimalloc raw (M cyc/s) | node / mi |
|------:|------:|------:|------:|
| 8 | 303 | 138 | 2.2x |
| 16 | 326 | 219 | 1.5x |
| 32 | 317 | 211 | 1.5x |
| 64 | 313 | 212 | 1.5x |
| 128 | 302 | 210 | 1.4x |
| 256 | 297 | 187 | 1.6x |
| 512 | 111 | 177 | 0.6x |
| 1024 | 66 | 125 | 0.5x |
| 4096 | 63 | 59 | 1.1x |

Reading it:

- **Small classes (≤ 256 B) — the point of the allocator — run ~300–325 M cyc/s, ~1.4–2.2x mimalloc even
  on the pre-split path.** That old path paid two `lock`-prefixed RMWs per cycle (`lock and` on allocate,
  `lock or` on free); the shipped split makes the owner's allocate/free plain non-atomic ops (`dev.py
  assembly show node_alloc_free_hotloop_probe`, see the
  [disassembly guide](../../../../../docs/guides/disassembly.md)), so the margin is now wider. Throughput is
  also **pattern-insensitive** — batch (alloc all, then free all) and interleaved (free-one / alloc-one)
  land within noise for node; mimalloc varies more with the pattern.
- **The large path (> 256 B) is *slower* than mimalloc**, not "tracking" it: it is mimalloc plus a 24 B
  header and a second layer of function-pointer indirection, so 512 B / 1024 B fall to ~0.5–0.6x. Expected
  — there is no bespoke node fast path above the small classes — but it means node is the wrong tool for
  anything that is not a small single object.

**Hardware sensitivity — measure, don't assume.** The margin's size varies by microarchitecture. An earlier
draft reported node at *half* mimalloc (~65 vs ~165 M cyc/s) on a Ryzen 9 5900X (Zen 3) laptop, where the
relative order *inverts*. So "node beats mimalloc" is true here and is not a law — it is the claim that must
be re-checked per architecture, which is why all fast-path variants ship as a benchmark
([`bench-node-design`](../../tests/benchmarks/node-allocation-design-benchmark.cc)) to compare on any
machine. The variant that wins there is the one this frontend ships (`step2_tls_diff`), and it is
uarch-dependent — re-run the design benchmark on new hardware.

Reproduce:

```bash
uv run dev.py --mirror-output test "bench-alloc (handle & node comparison)" --target clean-core-test --preset release-clang --timeout 0
uv run dev.py --mirror-output test "bench-alloc (steady-state small batch)" --target clean-core-test --preset release-clang --timeout 0
```
