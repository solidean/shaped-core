# node allocation

The node allocator ([node_allocation.hh](../../src/clean-core/memory/node_allocation.hh),
[node_allocation.cc](../../src/clean-core/memory/node_allocation.cc)) is a slab allocator for
**small, single objects** — list/map/set nodes, `unique_ptr` payloads, `unique_function` storage.
The goal is a **maximally cheap thread-local fast path** for these; it also gives two things the
general resource can't cheaply offer: an 8-byte owning handle (the class is derived from the *type*,
so nothing but a pointer is stored) and **wait-free cross-thread deallocation** (a single `atomic_or`,
no reference to the allocator needed).

It is the **less mature** of clean-core's two memory systems: several parts described in the header
are half-wired or leak by design. That immaturity is in the **lifecycle** (refill leaks, no trim), not the
fast path — which on current hardware already outruns the general allocator it aims to replace (see
[throughput](#throughput--where-it-actually-stands)). Those gaps are called out explicitly below. Read this
before relying on it under sustained churn.

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
- **Hot-path atomic removal** — split each slab's free bitmap into a local (owner-only, non-atomic) and a
  remote (atomic) half, so local allocate/free avoid the two `lock`-prefixed RMWs the fast path pays today;
  atomics stay only on genuine cross-thread frees. Step 1 (non-atomic allocate, atomic free retained)
  removes one lock with no API change; a further step removes the second at the cost of an owner check on
  free. See the throughput section.
- **Slab trim** — return fully-free slabs to the backing resource so high-watermark memory can be reclaimed.
- **Over-aligned (> 8 B) large nodes.**
- **Co-located refcounts** — the header notes a possible future `shared_ptr`-like variant storing the
  count next to the node ([node_allocation.hh:264](../../src/clean-core/memory/node_allocation.hh#L264)).
- **`any_node_allocation` reserved bytes** — it has 7 padding bytes earmarked for future use
  ([node_allocation.hh:488](../../src/clean-core/memory/node_allocation.hh#L488)).

## Throughput — where it actually stands

**Speed matters, but it is not the reason this allocator exists** — the 8-byte handle and the stateless
wait-free free are (see Model). On throughput it is already competitive-to-ahead for the small classes it
targets, which is the *opposite* of what an earlier draft of this section claimed.

From the [handle & node comparison benchmark](../../tests/benchmarks/allocation-benchmark.cc)
(`bench-alloc (handle & node comparison)`, manual; 32 live nodes to stay within one slab and avoid the
leaky refill), cross-checked against the batch/interleaved
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

- **Small classes (≤ 256 B) — the point of the allocator — run ~300–325 M cyc/s, ~1.4–2.2x mimalloc.** The
  fast path is exactly two `lock`-prefixed RMWs per cycle: `lock and` to claim a slot on allocate, `lock or`
  to release it on free (confirmed with `dev.py assembly show`, see the
  [disassembly guide](../../../../../docs/guides/disassembly.md)). On this Zen 4 part those locked ops are
  cheap enough that the design's lower per-op overhead (nothing stored but a pointer, class recovered from
  it) wins outright. Throughput is also **pattern-insensitive** — batch (alloc all, then free all) and
  interleaved (free-one / alloc-one) land within noise for node; mimalloc varies more with the pattern.
- **The large path (> 256 B) is *slower* than mimalloc**, not "tracking" it: it is mimalloc plus a 24 B
  header and a second layer of function-pointer indirection, so 512 B / 1024 B fall to ~0.5–0.6x. Expected
  — there is no bespoke node fast path above the small classes — but it means node is the wrong tool for
  anything that is not a small single object.

**Hardware sensitivity — measure, don't assume.** The whole margin rests on the cost of those two locked
RMWs, which varies by microarchitecture. An earlier draft reported node at *half* mimalloc (~65 vs ~165 M
cyc/s); those numbers were from a Ryzen 9 5900X (Zen 3) laptop, where the relative order *inverts*. So
"node beats mimalloc" is true here and is not a law — it is exactly the claim that must be re-checked per
architecture, which is why the fast-path variants are being built to ship and compare everywhere.

**The optimization that would widen the margin** is the local/remote bitmap split (see TODO): make local
allocate/free non-atomic and pay atomics only on genuine cross-thread frees. This supersedes the older
"the atomics are the target" framing — the atomics are the target, but the fix is a data-structure split,
not a micro-tweak, and the allocator is already ahead while it waits.

Reproduce:

```bash
uv run dev.py --mirror-output test "bench-alloc (handle & node comparison)" --target clean-core-test --preset release-clang --timeout 0
uv run dev.py --mirror-output test "bench-alloc (steady-state small batch)" --target clean-core-test --preset release-clang --timeout 0
```
