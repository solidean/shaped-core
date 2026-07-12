# node allocation

The node allocator ([node_allocation.hh](../../src/clean-core/memory/node_allocation.hh),
[node_allocation.cc](../../src/clean-core/memory/node_allocation.cc)) is a slab allocator for
**small, single objects** — list/map/set nodes, `unique_ptr` payloads, `unique_function` storage.
The goal is a **maximally cheap thread-local fast path** for these; it also gives two things the
general resource can't cheaply offer: an 8-byte owning handle (the class is derived from the *type*,
so nothing but a pointer is stored) and **wait-free cross-thread deallocation** (a single `atomic_or`,
no reference to the allocator needed).

It is the **less mature** of clean-core's two memory systems: several parts described in the header
are half-wired or leak by design, and — as the [throughput](#throughput--the-goal-and-where-it-stands-today)
section shows — the fast path does not yet beat the general allocator it aims to replace. Those gaps
are called out explicitly below. Read this before relying on it under sustained churn.

## Model

Allocations are bucketed into power-of-two **size classes**: class index `i` ⇒ size `2^i` bytes.
The class for a type is `bit_width(max(sizeof(T), alignof(T)) - 1)`, so both size and alignment are
satisfied. Classes `0..8` (1 B … 256 B) are the fast **small path**; anything larger takes a separate
header-backed **large path**.

Each small class is served from **slabs**: a `64 * class_size` block, aligned to its own size, so the
slab base is recovered from any interior pointer by `ptr & ~(slab_size - 1)` — a single AND, no
metadata lookup. Each slab begins with a 16-byte prefix:

- offset 0: a `u64` **free bitmap**, one bit per slot (1 = free).
- offset 8: a **next-slab pointer** (the slab ring, see gotchas).

The prefix consumes whole slots, so usable capacity depends on class size:

| class size | slots blocked by prefix | usable slots (of 64) |
|---:|---:|---:|
| 1 B | 16 | 48 |
| 8 B | 2 | 62 |
| ≥ 16 B | 1 | 63 |

**Allocation** (thread-owned, *not* thread-safe): find a free bit via `count_trailing_zeroes`, clear
it with an atomic `fetch_and`, return `base + slot * class_size`. **Deallocation** (any thread,
wait-free): recover the base, set the slot's bit with `atomic_or`. Because the class index comes from
the pointer + `sizeof`/`alignof`, free needs no allocator or resource — this is what makes remote free
cheap and stateless.

Handles: `node_allocation<T>` (typed, move-only), `any_node_allocation` (type-erased pointer + deleter
+ class index, for wrappers with no natural base class), and `poly_node_allocation<T, NodeTraits>` (the
class index is produced dynamically by user traits, enabling polymorphism).

The large path (> 256 B) allocates from `default_memory_resource` (mimalloc) with a 24-byte header
`[size][alignment][resource*]`; free reads the header back. So a large node is *just mimalloc plus a
header* — not the slab machinery at all.

## Gotchas

- **Slab refill leaks by design — the headline issue.** When a class runs out of free slots,
  `system_refill_slabs_and_allocate_node_bytes`
  ([node_allocation.cc:124–130](../../src/clean-core/memory/node_allocation.cc#L124-L130)) allocates a
  *fresh* slab, wires it as a **self-cycle**, and overwrites the class head — **dropping the previous
  slab ring entirely**. The old slabs, and every slot ever freed in them, are never revisited and never
  returned. Under a workload whose live set repeatedly exceeds one slab's capacity this is unbounded
  growth. It is marked `TODO`/`FIXME` in the code. Workloads that stay within a slab (the intended
  small-node case) never hit this.

- **The multi-slab ring is aspirational.** The next-pointer and the ring-walk in
  `allocate_node_bytes_non_fast`
  ([node_allocation.cc:198–226](../../src/clean-core/memory/node_allocation.cc#L198-L226)) are fully
  built to revisit multiple slabs of a class — but because refill only ever emits a self-cycle, the
  walk currently never finds a second slab and always falls through to another (leaking) refill. The
  machinery exists; nothing feeds it yet.

- **Slabs are never returned.** There is no trim path back to the backing resource, so the default node
  resource is effectively a grow-only arena, and a thread's TLS slabs leak when the thread exits.

- **Remote frees into a dropped slab are lost.** A wait-free free always succeeds (sets the bit), but if
  it lands in a slab that refill has already orphaned, that slot is never rediscovered for reuse.

- **Over-aligned large nodes are unsupported.** `system_allocate_node_bytes_large` asserts
  `alignment == 8` ([node_allocation.cc:43](../../src/clean-core/memory/node_allocation.cc#L43)); larger
  alignments are a `TODO`.

- **Alloc-heavy workloads re-walk the ring on each exhaustion**
  ([node_allocation.cc:194](../../src/clean-core/memory/node_allocation.cc#L194)) — no bookkeeping caches
  the last-known-full point, so exhaustion is O(ring) each time. A `TODO`.

- **Stale header comment.** `node_allocator::refill_slabs_and_allocate_node_bytes` is annotated
  `TODO: implement me` ([node_allocation.hh:229](../../src/clean-core/memory/node_allocation.hh#L229)),
  but it *is* implemented — it delegates to the resource
  ([node_allocation.cc:175](../../src/clean-core/memory/node_allocation.cc#L175)). The comment is
  misleading; the missing piece is the *non-leaking* refill strategy inside the resource, not this method.

- **Allocation is single-threaded per allocator.** A `node_allocator` is a thread-local cache and must
  not allocate from two threads; only *free* is concurrency-safe. A `node_memory_resource` that wants to
  serve many threads does so by handing out different allocators (e.g. via TLS), which the default does.

## TODO / not yet implemented

- **Non-leaking refill** — retain non-empty previous slabs in the ring instead of orphaning them; this is
  the fix for the headline leak and would make the existing ring-walk actually useful.
- **Slab trim** — return fully-free slabs to the backing resource so high-watermark memory can be reclaimed.
- **Over-aligned (> 8 B) large nodes.**
- **Co-located refcounts** — the header notes a possible future `shared_ptr`-like variant storing the
  count next to the node ([node_allocation.hh:264](../../src/clean-core/memory/node_allocation.hh#L264)).
- **`any_node_allocation` reserved bytes** — it has 7 padding bytes earmarked for future use
  ([node_allocation.hh:488](../../src/clean-core/memory/node_allocation.hh#L488)).

## Throughput — the goal, and where it stands today

**Speed is the whole point of this allocator** — an ultra-cheap thread-local fast path is its reason
to exist. It is not there yet: today the small-class slab path is *slower* than mimalloc, and closing
that gap is the main work remaining. The numbers below are the current baseline, not the target.

From the [handle & node comparison benchmark](../../tests/benchmarks/allocation-benchmark.cc)
(`bench-alloc (handle & node comparison)`, manual; 32 live nodes to stay within one slab and avoid the
leaky refill). Metric is **millions of alloc+free cycles per second — higher is better** (one cycle =
one free + one allocate). Release build, Ryzen 9 5900X; single-run, small rows noisy (~10%):

| size (B) | node (M cyc/s) | mimalloc raw (M cyc/s) |
|------:|------:|------:|
| 8 | 64.0 | 60.4 |
| 16 | 59.0 | 171.3 |
| 32 | 65.1 | 172.5 |
| 64 | 70.3 | 166.3 |
| 128 | 68.8 | 165.9 |
| 256 | 70.6 | 141.5 |
| 512 | 96.8 | 146.3 |
| 1024 | 50.1 | 68.8 |
| 4096 | 41.0 | 36.0 |

Reading the baseline:

- **The small-class slab path runs ~55–70 M cyc/s — roughly half of mimalloc** at the same sizes. The
  three atomics per cycle (an atomic bitmap load + `fetch_and` to allocate, an `atomic_or` to free)
  currently cost more than mimalloc's lock-free thread-local free list. That per-cycle atomic traffic is
  the obvious first target for the fast path.
- **The large path (> 256 B) is just mimalloc + a 24 B header**, so the 512 B row (~97 M cyc/s) tracks
  mimalloc rather than the slab path — above the small classes there is no bespoke node fast path to win
  or lose on.
- **Where it already pays off** — even at today's speed — is footprint and concurrency: an 8-byte handle
  (no stored size or resource), stateless wait-free cross-thread free, and no per-object allocator
  metadata. The aim is to keep those while making the fast path decisively beat the general allocator.

Reproduce:

```bash
uv run dev.py test "bench-alloc (handle & node comparison)" --target clean-core-test --preset release-clang --timeout 0
```
