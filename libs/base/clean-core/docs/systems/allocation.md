# cc::allocation

`cc::allocation<T>` ([allocation.hh](../../src/clean-core/memory/allocation.hh)) is the
owning storage handle underneath every contiguous heap container — `array<T>`, `vector<T>`,
`devector<T>`. It exists so the sharp, failure-prone parts (allocation ownership, resizing,
alignment, object lifetime) live in one place and the containers only supply *policy* (when
the live window moves and when growth happens).

This doc is the system-level view: the model, the resource interface, and — importantly — the
gaps between the header prose and what the code actually does today. For the API surface see the
[cheat sheet](../../cheat-sheet.md); for numbers see the [handle vs raw benchmark](#throughput) below.

## What it models

An allocation tracks two ranges independently:

- **owned bytes** — `[alloc_start, alloc_end)`, the block returned by the memory resource.
- **live objects** — `[obj_start, obj_end)`, the constructed `T`s inside that block.

Everything outside the live window is dead storage. Capacity is *derived*, not stored: the
spare room is the bytes between `obj_end` and `alloc_end` (and, for front-growing devectors,
between `alloc_start` and `obj_start`). Keeping the full byte bounds — rather than the classic
`(ptr, size, capacity)` triple — is what lets the handle reuse a block without reallocating,
realign by shifting the live window, and round-trip capacity.

The six members and their invariants:

| member | meaning |
|---|---|
| `T* obj_start` | first live object; **always aligned to `alignof(T)`**, even when empty |
| `T* obj_end` | one past last live object (exclusive); same alignment invariant |
| `cc::byte* alloc_start` | base pointer to pass back to the resource on free |
| `cc::byte* alloc_end` | one past the owned bytes (exclusive) |
| `isize alignment` | alignment used to allocate the block (needed for correct deallocation) |
| `memory_resource const* custom_resource` | owning resource, or `nullptr` ⇒ global default |

Invariant chain: `alloc_start <= obj_start <= obj_end <= alloc_end`. The all-zero state is a
valid empty allocation (no bytes, no objects, default resource implied), so a default-constructed
handle needs no special-casing.

## The resource interface

Memory comes from a polymorphic `cc::memory_resource` — a POD struct of function pointers (no
vtables, no non-trivial ctor), data-segment resident so the default-resource pointer is valid
during static init. The resource is stored *in the handle*, not as a template argument; this is
the deliberate escape from std-style allocator-typed container variants and allocator-propagation
rules. A container selects a non-default allocator by seeding an empty allocation carrying that
resource.

- `allocate_bytes(out_ptr, min, max, align, ud)` — allocates in `[min, max]`, returns the
  actual size. `min == 0` ⇒ `nullptr`, size 0. `min > 0` failure is **fatal** (assert/terminate).
  Size-class allocators may return more than `min` so containers claim the rounding for free.
- `try_allocate_bytes(...)` — same, but returns `-1` on failure instead of terminating (optional).
- `deallocate_bytes(p, bytes, align, ud)` — `bytes`/`align` must match the allocation.
- `try_resize_bytes_in_place(...)` — grow/shrink without moving (optional; see the gotcha below).

Two resources ship: `cc::default_memory_resource` (mimalloc-backed, the default) and
`cc::system_memory_resource` (platform `malloc`/`_aligned_malloc`, the explicit opt-out). Pass the
latter as a custom resource to bypass mimalloc for one allocation.

## Gotchas

- **The adopt/release escape hatch works today; the convenience API and `retype` do not.** Zero-copy
  transfer between container types is already possible: `container.extract_allocation()` moves the
  `cc::allocation<T>` out ([vector.hh:150](../../src/clean-core/container/vector.hh#L150)), and any
  container's `create_from_allocation(cc::move(alloc))` factory adopts one
  ([allocating_container.hh:1116](../../src/clean-core/container/impl/allocating_container.hh#L1116)) —
  so e.g. `array::create_from_allocation(vec.extract_allocation())` re-homes storage with no element
  copy. What's missing is (a) sugar around that round-trip and (b) `retype` — the fallible
  reinterpretation to a different element type for trivially copyable payloads that the header prose
  promises (allocation.hh
  [133–146](../../src/clean-core/memory/allocation.hh#L133-L146),
  [200–208](../../src/clean-core/memory/allocation.hh#L200-L208)) but which has **no method anywhere
  yet**. See [TODO](#todo--not-yet-implemented).

- **In-place resize is resource-dependent and weak.** `system_memory_resource` always returns `-1`
  (malloc has no in-place resize). mimalloc "succeeds" only when the request already fits the block's
  existing usable size (`mi_usable_size`) — it never grows a block past its size class. So
  `try_resize_alloc_inplace` growth mostly just *claims the slack the allocator already rounded up
  to*; a genuine larger request still reallocates and moves. This is a backing-allocator limitation,
  not a bug, but do not rely on cheap in-place growth.

- **const-correctness is the caller's job.** `obj_span()` hands out a mutable `span<T>` from a
  `const` handle by design ([allocation.hh:246](../../src/clean-core/memory/allocation.hh#L246));
  the handle does not police element mutability.

- **`create_uninitialized_unsafe` skips the safety checks.** Unlike `create_uninitialized` (which
  static-asserts trivial copy + trivial destroy), the `_unsafe` variant treats raw storage as live
  with no checks — the caller must guarantee initialization-before-read and a trivial (or manually
  driven) destructor.

- **`resize_alloc` realignment reallocates + moves.** Raising alignment above the current block's
  alignment forces a fresh allocation and a move-construct of the live objects (so `T` must be
  move-constructible on that path).

- **Ring buffers are out of scope.** Once data wraps, the live region is segmented and no longer
  matches the single contiguous `[obj_start, obj_end)` window; wrap-around containers cannot be
  built on this handle.

- **The handle is deliberately fat** (six members vs a minimal vector header). The extra metadata
  buys correct deallocation, pooling/reuse, realignment, and the (planned) cross-container interop.

## TODO / not yet implemented

- **`retype`** — fallible reinterpretation of a cleared allocation to a different (trivially copyable)
  element type is described in the header but has no implementation. This is the main gap between the
  doc-comment vision and the current handle.
- **Convenience API around extract/adopt** — the escape hatch works via the container
  `extract_allocation` / `create_from_allocation` pair, but the ergonomic "move this vector's storage
  into an array" surface is still to be built. **Cross-container-type moves without element copy also
  want direct test coverage** to pin the guarantee.
- **Cheap in-place growth** is bounded by the backing allocator and today only reclaims existing
  size-class slack — a documented limitation to revisit if a resource gains real `realloc`-without-move.

## Throughput

`cc::allocation<byte>` adds only bookkeeping on top of the memory resource, so it should track the
raw resource closely. The benchmark measures this: it churns 64 concurrently-live blocks over the
mimalloc default (each cycle frees the oldest and creates a fresh block, touching both ends), once
through the bare resource and once through the handle.

The metric is **millions of alloc+free cycles per second — higher is better** (one cycle = one free +
one allocate). Release build, Ryzen 9 5900X; single-run numbers, small rows noisy (~10%):

| size (B) | mimalloc raw (M cyc/s) | cc::allocation (M cyc/s) |
|------:|------:|------:|
| 8 | 60.4 | 131.8 |
| 16 | 171.3 | 131.7 |
| 32 | 172.5 | 130.5 |
| 64 | 166.3 | 129.7 |
| 128 | 165.9 | 115.4 |
| 256 | 141.5 | 121.3 |
| 512 | 146.3 | 111.2 |
| 1024 | 68.8 | 70.5 |
| 4096 | 36.0 | 41.6 |

The handle costs ~10–25% over the bare resource at small sizes (the extra work of computing the live
window and the move-assign destroy-then-adopt), and that overhead disappears by page size once the
allocation cost itself dominates. (The 8 B `mimalloc raw` cell is a noisy outlier — neighboring runs
land near 120–170.)

Source: [allocation-benchmark.cc](../../tests/benchmarks/allocation-benchmark.cc), the manual
`bench-alloc (handle & node comparison)` test. Reproduce:

```bash
uv run dev.py test "bench-alloc (handle & node comparison)" --target clean-core-test --preset release-clang --timeout 0
```
