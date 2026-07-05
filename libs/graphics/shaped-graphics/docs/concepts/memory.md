# Concept: memory

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [epochs](epochs.md).

Two orthogonal axes decide where a resource's GPU memory comes from and how long it lives:

- **Lifetime mode** — `lifetime_scope`: `persistent` vs `transient`. *When* the memory is valid.
- **Backing** — dedicated vs placed. *How* the memory is allocated.

An [`allocation_info`](../../src/shaped-graphics/allocation_info.hh) carries both and is passed to every
`create_*` call.

## Lifetime modes

The mode is a **hard contract, not a hint** — it changes when a resource is legal to use, not just how
the backend prefers to schedule memory.

**`persistent`** — the resource lives until its handles are released. This is the default, reached
through `ctx.persistent.create_*`. Normal shared-lifetime ownership.

**`transient`** — the resource is tied to an epoch and **expires when that epoch retires**. Using it
beyond that epoch is a hard error, and the backend may recycle the memory the moment the epoch is done.
Transient is for per-frame scratch (intermediate targets, staging) that never needs to outlive the work
that produced it.

Both modes still get in-flight GPU hazard tracking — that is orthogonal to lifetime. Hazard tracking
answers "has the GPU finished reading this?"; the lifetime mode answers "am I still allowed to name this
resource at all?".

## Backing: dedicated vs placed

**Dedicated** (`allocation_info::is_dedicated()`, null heap) — the resource owns its own allocation, a
"committed resource" in dx12 terms. Simple and self-contained; one allocation per resource.

**Placed** (`is_placed()`, non-null heap) — the resource is sub-allocated into a shared
[`memory_heap`](../../src/shaped-graphics/memory_heap.hh) at an offset, sharing that heap's single
underlying allocation. Placing many resources into one heap avoids per-resource allocation overhead and
lets the caller pack them however it likes.

A `memory_heap` is an **immutable factory for `allocation_info`** — it does not track what has been
allocated inside it. The caller's own allocator owns sub-allocation and tracking; the heap only reports
per-resource `memory_requirements` (backend alignment + occupied size, which may exceed the requested
size — this is what lets textures, whose real footprint the driver decides, share the path) and validates
an offset before minting the `allocation_info`. A handle back to the heap keeps it alive as long as any
placement references it.

Intended flow: query requirements → the caller's allocator picks an offset →
`heap.acquire_allocation_for_*(...)` → pass the `allocation_info` to the matching `create_*`.

## How the two axes relate

The axes are independent, but in practice:

- **Persistent** resources are where the placed-vs-dedicated choice matters most: long-lived resources
  are what you pack into shared heaps to control fragmentation and allocation count.
- **Transient** resources will usually be backed by a **placed linear allocation** on top of a heap — a
  bump allocator per epoch, reset when the epoch retires — rather than a dedicated allocation each frame.

## Status

Both **dedicated** and **placed** buffer backing work on dx12: `ctx.persistent.create_memory_heap(size)`
mints a heap, and a placement's `allocation_info` routes `create_buffer` through `CreatePlacedResource`
(the buffer holds a handle to its heap so the heap outlives the placement). The vulkan backend still
stubs both. The transient scope is not yet exposed — only `ctx.persistent` exists — so the per-epoch
bump allocator that will sit on top of a heap is still to come.

## See also

- [allocation_info.hh](../../src/shaped-graphics/allocation_info.hh) — the value type and `lifetime_scope`.
- [memory_heap.hh](../../src/shaped-graphics/memory_heap.hh) — the heap factory and `memory_requirements`.
- [epochs](epochs.md) — the epoch retirement that bounds a transient resource's lifetime.
