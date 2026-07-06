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
- **Transient** resources are backed by a **placed linear allocation** on top of a heap — a bump
  allocator reclaimed per epoch — rather than a dedicated allocation each frame.

## Transient allocation

`ctx.transient.create_buffer(size, usage)` sub-allocates from a **per-epoch bump allocator** over one
DEFAULT heap the transient scope owns: a monotonic head hands out placement offsets, and **resets to 0
whenever the epoch changes**. Successive epochs therefore alias the same storage — which is not only
safe but desired. It is safe because a single direct queue executes each epoch's GPU work before the
next's, so epoch N's transient memory is finished before epoch N+1 (which resets to 0) can touch it; the
epoch boundary is the barrier. It is desired because a one-epoch-sized heap serves any pipelining depth,
which is smaller and kinder to caches than a ring sized for every in-flight frame. A request larger than
the budget falls back to a dedicated (committed) allocation. The budget defaults to 128 MiB and is a
single shared pool for all transient resources (buffers today, textures once wired), set with
`ctx.transient.set_budget`. That setter is deferred: it records a pending budget and returns without
touching the GPU; the **next** `advance_epoch` applies it by draining in-flight work and resizing the
heap, so the change is predictable and never mid-epoch.

> This bump-reset-and-alias scheme is specific to buffers, whose transient contents are only ever
> GPU-touched. Transient **descriptors** are different: they are written by the CPU at group creation,
> so a slot cannot be reused until the epoch that wrote it retires. They therefore use a per-epoch ring,
> not a bump-reset — see [bindings](bindings.md). Placing textures in the shared heap additionally needs
> the backend heap to allow buffers and textures together (on dx12: Resource Heap Tier 2, dropping the
> current `ALLOW_ONLY_BUFFERS` flag) plus texture memory-requirement queries — future work.

## Expiry

Storage that a lifetime scope reclaims must not be named afterwards, so `sg::buffer` carries explicit
expiry state, independent of how it was allocated:

- `is_expired()` / `is_valid()` — public: whether the buffer still names live storage.
- `expire()` — release the storage now (deferred until no longer in flight), even while handles remain.

A **transient** buffer is auto-expired when its epoch advances (its bump storage is about to be reused).
A **persistent** buffer can be expired **explicitly** to free memory early without hunting down every
`shared_ptr`. Using an expired buffer in a transfer or a binding is a hard error (asserted). A transient
`binding_group` has the analogous rule — binding one past its epoch asserts, since the ring may have
recycled its descriptor slots.

## Status

Both **dedicated** and **placed** buffer backing work on dx12: `ctx.persistent.create_memory_heap(size)`
mints a heap, and a placement's `allocation_info` routes `create_buffer` through `CreatePlacedResource`
(the buffer holds a handle to its heap so the heap outlives the placement). `ctx.transient` exposes
per-epoch buffers and binding groups (above). The vulkan backend stubs all of it (heaps, placed
resources, transient) until its own milestone.

## See also

- [allocation_info.hh](../../src/shaped-graphics/allocation_info.hh) — the value type and `lifetime_scope`.
- [memory_heap.hh](../../src/shaped-graphics/memory_heap.hh) — the heap factory and `memory_requirements`.
- [epochs](epochs.md) — the epoch retirement that bounds a transient resource's lifetime.
