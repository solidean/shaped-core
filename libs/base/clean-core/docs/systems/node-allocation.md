# node allocation

The node allocator is a slab allocator for **small, single objects** — list/map/set nodes, `unique_ptr` payloads, `unique_function` storage.
It lives in [node_allocation.hh](../../src/clean-core/memory/node_allocation.hh) and [node_allocation.cc](../../src/clean-core/memory/node_allocation.cc).
The goal is a **maximally cheap thread-local fast path** for these, and it gives two things the general resource cannot cheaply offer.
An **8-byte owning handle**: the class is derived from the *type*, so nothing but a pointer is stored.
And **stateless cross-thread deallocation**: the freeing thread finds everything from the pointer, so no reference to the allocator is needed.

It is the **less mature** of clean-core's two memory systems, but the fast path and the full slab lifecycle are now resolved.
The fast path is **two compile-time frontends** selected by `CC_HAS_THREADS` — see [Model](#model).
The refill path retains previous slabs in a ring instead of orphaning them, and a thread's slabs are handed off and adopted across thread exit rather than leaked.
Surplus fully-free slabs are trimmed back to the backing resource.
[Slab lifecycle](#slab-lifecycle-across-threads) covers all three.
The design target is a **fixed set of long-lived threads**, and that workload pays nothing for any of the lifecycle machinery.
A thread-churning server stays correct and bounded but is second class: it pays a rare per-class lock on thread exit and on slab adoption.

## Model

Allocations are bucketed into power-of-two **size classes**: class index `i` gives size `2^i` bytes.
The class for a type is `bit_width(max(sizeof(T), alignof(T)) - 1)`, so both size and alignment are satisfied.
Classes `0..8` (1 B … 256 B) are the fast **small path**; anything larger takes a separate header-backed **large path**.

Each small class is served from **slabs**: a `64 * class_size` block, aligned to its own size.
So the slab base is recovered from any interior pointer by `ptr & ~(slab_size - 1)` — a single AND, no metadata lookup.
Data starts at slab offset 0, and slots that overlap the slab's metadata are permanently blocked by a per-class **consteval seed mask** (`node_compute_seed_local_freemap`).
The metadata layout is **frontend-dependent**, on `CC_HAS_THREADS`:

- **Threaded** (native, wasm+pthreads): `local` free bitmap `@0` (u64), `owner_id` `@8` (u32),
  next-slab pointer `@16`; a second **`remote`** free bitmap on the 2nd cache line `@64` (for classes
  whose slab spans two lines, i.e. ≥ 2 B) or `@24` for the single-line 1 B class.
- **Single-threaded** (wasm without `-pthread`): `local` free bitmap `@0`, next-slab pointer `@8`. No
  owner/remote — the second cache line is not touched.

The metadata consumes whole slots, so usable capacity depends on class size and frontend:

| class size | usable (threaded) | usable (single) |
|---:|---:|---:|
| 1 B | 36 | 48 |
| 2 B | 50 | 56 |
| 8 B | 60 | 62 |
| ≥ 16 B | ~62 | 63 |

The **fast path is two frontends behind an unchanged API**, selected at compile time:

- **Threaded (`step2_tls_diff`).** The owning thread allocates and frees **non-atomically** into `local`: find a free bit via `count_trailing_zeroes` and clear it; a free sets it.
  A *genuinely remote* thread frees with a single `atomic_or` into `remote`, which sits on a separate cache line so it never invalidates the owner's hot `local` line.
  When `local` empties, the owner drains `remote` into it via one atomic `exchange` on the cold path.
  The owner is identified by a process-unique, never-recycled `owner_id`, stamped into the slab at hydrate and compared against a thread-local token on free.
- **Single-threaded.** Plain non-atomic `and`/`or` on the one `local` bitmap — no owner, no remote, no
  atomics at all.

Because the class index comes from the pointer + `sizeof`/`alignof`, **free needs no allocator or
resource** — this is what makes cross-thread free stateless.

Handles: `node_allocation<T>` (typed, move-only), `any_node_allocation` (type-erased pointer + deleter
+ class index, for wrappers with no natural base class), and `poly_node_allocation<T, NodeTraits>` (the
class index is produced dynamically by user traits, enabling polymorphism).

The large path (> 256 B) allocates from `default_memory_resource` (mimalloc) with a 24-byte header `[size][alignment][resource*]`, and free reads the header back.
So a large node is *just mimalloc plus a header* — not the slab machinery at all.

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

- **owned(T).** The hydrating thread `T` owns the slab, with its process-unique, never-recycled `owner_id` stamped in.
  `T` allocates and frees plain-local; every other thread routes its frees to `remote`.
- **Abandonment (thread exit).** When a thread's `thread_local` allocator is destroyed, it reclaims its ring **through a resource hook** (`node_memory_resource::reclaim_slabs`).
  The system resource implements it; a resource without the hook leaks its slabs on exit instead.
  Per class, under a per-class lock: each slab's `remote` is drained into `local`, **fully-free** slabs go back to the backing resource, and the rest are pushed onto a per-class **orphan bin**.
  Because `owner_id` is never recycled and the owner only stops owning by *dying*, every subsequent free into an orphaned slab is guaranteed to take the `remote` path.
  So no plain-local writer can race the future adopter.
- **Adoption (refill).** Before mallocing a fresh slab, refill pops one orphan of the class if present.
  It re-stamps `owner_id` to the adopting thread, drains the slab's accumulated `remote` frees, and splices it into the ring.
  `owner_id` is only ever *stamped* atomically — an `atomic_ref` store on the cold refill path — while the hot free-path read stays a plain `u32` load.
- **Trim.** On the cold allocation path only, never on free, a per-class counter gates a rare O(ring) sweep that returns surplus fully-free slabs to the backing resource.
  One fully-free spare is kept, so a steady working set never re-mallocs.
  A stable set of live nodes never produces a fully-free slab, so a long-lived thread never actually trims.

The per-class orphan bins are `constinit` spinlocks rather than `std::mutex`, so they are never torn down and the main thread's allocator can reclaim safely even during static destruction.
Ownership is only ever transferred at thread exit.
There is deliberately **no mid-life slab migration** between live threads: that would force an atomic `owner_id` and cost the hot path.

## Gotchas

- **`owner_id` is a `u32` and is never recycled.** Abandonment reclaims *slabs*, not *ids*, so the counter stays monotonic.
  That is what keeps a live id from ever being reused, and a free from being miscategorized.
  An assert fires past ~4 B threads *ever* created: a fixed thread pool never wraps, and a server that spawns billions of threads over its lifetime would.
  Accepted, and loud when hit — see [philosophy.md](../../../../../docs/philosophy.md), "fail loud".

- **Cross-thread frees into an orphaned-but-never-adopted slab accumulate in `remote`.**
  If a thread hands out nodes and exits, and *no* later thread ever allocates that class again, the orphaned slab sits in its bin holding the still-live nodes.
  Frees keep landing in `remote` and are only reclaimed once the slab is adopted.
  Bounded at one bin per class, so not a leak in the grow-unbounded sense — but worth knowing.

- **Alloc-heavy workloads re-walk the ring on each exhaustion** ([node_allocation.cc](../../src/clean-core/memory/node_allocation.cc)).
  No bookkeeping caches the last-known-full point, so exhaustion is O(ring) every time.

- **Allocation is single-threaded per allocator.** A `node_allocator` is a thread-local cache and must not allocate from two threads; only *free* is concurrency-safe.
  A `node_memory_resource` that serves many threads does so by handing out different allocators, e.g. via TLS, which is what the default does.

- **Slab metadata layout is a whole-build switch.** The frontend, and so the slab layout, is chosen by `CC_HAS_THREADS` at compile time, so slabs never cross frontends.
  Do not mix objects built with and without threads in one binary.

Still open:

- **Cheaper ring re-walk** — cache the last-known-full point instead of an O(ring) scan per exhaustion.
- **`any_node_allocation` reserved bytes** — it has 7 padding bytes earmarked for future use.
- **A leaked node is invisible to a leak checker** — the slab is the allocation, so a dead slot is unused bytes in a live block.
  A build mode routing every class through the large-node path would fix it; [TODO](../TODO.md) has the shape.

**Confidence gaps — `TODO`s before the cross-thread guarantee is *proven*, not just *argued*:**

- **The concurrency correctness is reason-only.** The multi-thread tests use a *single, non-contending* helper thread.
  Nothing genuinely contends the relaxed atomics — `remote` `fetch_or`, drain `exchange`, adoption `owner` store — or the orphan-bin handoff.
  There is **no ThreadSanitizer** in this repo; the sanitizer presets are ASan+UBSan only.
  The real gate is a **contended, multi-generation stress test** run under a **TSan preset** (Linux/macOS/CI).
  That means threads that alloc, hand nodes to siblings, free siblings' nodes under contention, then exit — asserting bounded memory and all-nodes-freeable across generations.
  Until then, treat "correct under real concurrency" as believed, not demonstrated.
- **Single-threaded trim is compile-checked, not run.** The `CC_HAS_THREADS==0` frontend builds
  (`emscripten-release`) but the trim path there has not been executed under test.

## Throughput — where it actually stands

**Speed matters, but it is not the reason this allocator exists** — the 8-byte handle and the stateless wait-free free are (see Model).
On throughput it is already competitive-to-ahead for the small classes it targets.

From the [handle & node comparison benchmark](../../tests/benchmarks/allocation-benchmark.cc)
(`bench-alloc (handle & node comparison)`, manual). Metric is **millions of alloc+free cycles per second —
higher is better** (one cycle = one free + one allocate). Release build (`release-clang`),
**Ryzen 9 7950X3D**, representative run:

| size (B) | node (M cyc/s) | mimalloc raw (M cyc/s) | node / mi |
|------:|------:|------:|------:|
| 8 | 340 | 134 | 2.5x |
| 16 | 333 | 205 | 1.6x |
| 32 | 334 | 202 | 1.7x |
| 64 | 333 | 202 | 1.6x |
| 128 | 333 | 195 | 1.7x |
| 256 | 419 | 190 | 2.2x |
| 512 | 100 | 166 | 0.6x |
| 1024 | 64 | 126 | 0.5x |
| 4096 | 61 | 59 | 1.0x |

Reading it:

- **Small classes (≤ 256 B) — the point of the allocator — run ~330–420 M cyc/s, ~1.6–2.5x mimalloc.**
  The owner's allocate and free are plain non-atomic bitmap ops; a `lock` appears only when *another* thread
  frees (see the codegen below).
- **The large path (> 256 B) is *slower* than mimalloc**, not "tracking" it: it is mimalloc plus a 24 B header and a second layer of function-pointer indirection, so 512 B / 1024 B fall to ~0.5–0.6x.
  Expected, since there is no bespoke node fast path above the small classes — but it means node is the wrong tool for anything that is not a small single object.

### Hot-path codegen

From `dev.py assembly show node_alloc_free_hotloop_probe --preset release-clang` (threaded frontend, 16 B;
see the [disassembly guide](../../../../../docs/guides/disassembly.md)). The whole owner path is non-atomic;
the only `lock` is on the remote branch a same-thread workload never takes:

```text
; --- allocate (owner) ---
mov   r8,  [rcx + slab_base_off]   ; the class's current slab, cached in the allocator
mov   r9,  [r8]                    ; local free bitmap
tzcnt rax, r9                      ; lowest free slot
btr   r9,  rax                     ; clear it
mov   [r8], r9                     ; write the bitmap back           <- no lock
;     (ptr = slab + slot*size)

; --- free ---
mov   r11d, [base + 8]             ; the slab's owner_id
cmp   r11d, [owner_token]          ; == this thread's token?  (raw TLS read, no lazy-init branch)
jne   .remote
or    [base], bit                  ; owner: OR the slot back          <- no lock
.remote:
lock  or [base + remote_off], bit  ; other thread: the ONLY atomic in the hot path
```

### The design benchmark carries the real allocator

[`bench-node-design`](../../tests/benchmarks/node-allocation-design-benchmark.cc) sweeps eight idealized fast-path variants, one inline mini-allocator per lock-removal strategy.
**It is compiled out in the tree** — set `CC_BENCH_NODE_ALLOCATION_DESIGN` to 1 at the top of that file to build and run it.
The sweep costs ~9 s of compile time on every preset, which is not worth paying for a settled question; docs/notes/build-times.md has the measurement.
`mimalloc` and `system` run alongside them as reference lines.
It also drives a **`node` line over the real `cc::node_allocator`**.
On this machine the shipped `node` line **meets or beats** the idealized `step2_tls_diff` variant it implements, at every size, so the shipped code pays no penalty over the design it was chosen from.
The comparison stays honest because the idealized variants route their (never-taken) cold refill through an **opaque out-of-TU call**, forcing a slab-base reload every allocation.
That is exactly what the real allocator does, since its cold path lives in another TU.
Without it the mocks hoist a single fixed slab into a register and report a number the real allocator cannot reach.

**Hardware sensitivity — measure, don't assume.**
The margin's size varies by microarchitecture: on a Ryzen 9 5900X (Zen 3) laptop the relative order *inverts*, with node measuring at about *half* mimalloc.
So "node beats mimalloc" is true here and is not a law, which is why all fast-path variants ship as a benchmark to re-check on any machine.
The winning variant is uarch-dependent; the one this frontend ships is `step2_tls_diff`.

Reproduce (and regenerate the design-benchmark SVGs):

```bash
uv run dev.py --mirror-output test "bench-alloc (handle & node comparison)" --target clean-core-test --preset release-clang --timeout 0
# then, with CC_BENCH_NODE_ALLOCATION_DESIGN set to 1:
uv run libs/base/clean-core/scripts/plot-node-allocation-design.py --out .   # runs bench-node-design, writes two SVGs
```
