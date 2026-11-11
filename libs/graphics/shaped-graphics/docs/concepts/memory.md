# Concept: memory

Two orthogonal axes decide where a resource's GPU memory comes from and how long it lives:

- **Lifetime mode** (`lifetime_scope`) — `persistent` vs `transient`, deciding *when* the memory is valid.
- **Backing** — dedicated vs placed, deciding *how* the memory is allocated.

An [`allocation_info`](../../src/shaped-graphics/memory/allocation_info.hh) carries both and is passed to every
`create_*` call.

## Lifetime modes

The mode is a **hard contract, not a hint** — it changes when a resource is legal to use, not just how
the backend prefers to schedule memory.

**`persistent`** — the resource lives until its handles are released.
This is the default, reached through `ctx.persistent.create_*`: normal shared-lifetime ownership.

**`transient`** — the resource is tied to an epoch and **expires when that epoch retires**.
Using it beyond that epoch is a hard error, and the backend may recycle the memory the moment the epoch is done.
Transient is for per-frame scratch — intermediate targets, staging — that never needs to outlive the work that produced it.

Both modes still get in-flight GPU hazard tracking, which is orthogonal to lifetime.
Hazard tracking answers "has the GPU finished reading this?"; the lifetime mode answers "am I still allowed to name this resource at all?".

## Backing: dedicated vs placed

**Dedicated** (`allocation_info::is_dedicated()`, null heap) — the resource owns its own allocation, a "committed resource" in dx12 terms.
Simple and self-contained, at one allocation per resource.

**Placed** (`is_placed()`, non-null heap) — the resource is sub-allocated into a shared [`memory_heap`](../../src/shaped-graphics/memory/memory_heap.hh) at an offset.
It shares that heap's single underlying allocation.
Packing many resources into one heap avoids per-resource allocation overhead and lets the caller lay them out as it likes.

A `memory_heap` is an **immutable factory for `allocation_info`** — it does not track what has been allocated inside it.
The caller's own allocator owns sub-allocation and tracking.
The heap only reports per-resource `memory_requirements` and validates an offset before minting the `allocation_info`.
Those requirements are backend alignment plus occupied size, which may exceed the requested size — and that is what lets textures, whose real footprint the driver decides, share the path.
A handle back to the heap keeps it alive as long as any placement references it.

Intended flow: query requirements → the caller's allocator picks an offset →
`heap.acquire_allocation_for_*(...)` → pass the `allocation_info` to the matching `create_*`.

## How the two axes relate

The axes are independent, but in practice:

- **Persistent** resources are where the placed-vs-dedicated choice matters most: long-lived resources
  are what you pack into shared heaps to control fragmentation and allocation count.
- **Transient** buffers and textures are backed by a **placed linear allocation** on top of a heap — a bump allocator reclaimed per epoch — rather than a dedicated allocation each frame.
  Creating and freeing a texture every frame is what that avoids: on a laptop RTX 5070 each one blocked in the driver for 1 to 3 ms, and Sponza's hybrid frame created six.

## Transient allocation

`ctx.transient.create_raw_buffer(size, usage)` sub-allocates from a **per-epoch bump allocator** over one DEFAULT heap the transient scope owns.
`ctx.transient.create_texture_2d(...)` does the same over a second heap of its own, since a heap holds one kind of resource (`memory_heap_contents`).
A monotonic head hands out placement offsets and **resets to 0 whenever the epoch changes**, so successive epochs alias the same storage — which is not only safe but desired.
It is safe because a single direct queue executes each epoch's GPU work before the next's.
Epoch N's transient memory is therefore finished before epoch N+1 resets to 0 and touches it: the epoch boundary is the barrier.
On D3D12 the queue orders that by itself; on Vulkan, whose submission order carries no memory dependency, the first list submitted in each epoch begins with a barrier from every stage to every stage.
It is desired because a one-epoch-sized heap serves any pipelining depth, which is smaller and kinder to caches than a ring sized for every in-flight frame.
A request larger than the budget falls back to a dedicated (committed) allocation.
The buffer budget defaults to 128 MiB, set with `ctx.transient.set_budget`, and the texture budget to 256 MiB, set with `ctx.transient.set_texture_budget`.
That setter is deferred: it records a pending budget and returns without touching the GPU.
The **next** `advance_epoch` applies it by dropping the heap, and the next transient allocation creates one at the new size, so the change is predictable and never mid-epoch.
Nothing waits: every buffer placed in the old heap holds a handle to it, and its release is epoch-deferred, so the old heap lives exactly as long as the GPU work that reads it.
For an epoch or two both heaps exist; a caller for whom that matters lets the in-flight epochs retire before setting the budget.

> This bump-reset-and-alias scheme is specific to buffers and textures, whose transient contents are only ever GPU-touched.
> Transient **descriptors** are different: they are written by the CPU at group creation, so a slot cannot be reused until the epoch that wrote it retires.
> They therefore use a per-epoch ring rather than a bump-reset — see [bindings](bindings.md).

A placed transient texture **starts with undefined contents**: its memory held an earlier epoch's texture.
Its first barrier is the one from `undefined`, which on dx12 carries the discard that D3D12 requires to initialize a placed render target, and on Vulkan is the image's own initial transition.
Metal has no layouts and no such barrier: its first write defines the contents, as a render pass's clear does.
**Keep the epochs in flight bounded**, as a swapchain does: with dozens queued, every new frame's textures overlap memory the queued ones still use.
The NVIDIA driver then stalls when a placed render target is bound, for up to 2 s in a benchmark that queued 48 frames unthrottled.
Throttled to 2 in flight, the same frames recorded in 5 ms where dedicated textures had taken 15.
On an M4, `sg transient - per-frame render targets` took 31.8 ms for ten frames of ten 1920x1080 targets from the heap, 87 ms with every one dedicated, and 33 ms with the targets kept.
A texture falls back to a dedicated allocation where it cannot be placed:
- **The backend places no textures**: webgpu and dx12 below resource heap tier 2.
- **The heap cannot hold this one**: a multisampled texture on dx12, which needs a 4 MiB placement alignment, or a Vulkan image whose memory types exclude the heap's.
- **The epoch's window is full.**

## Expiry

Storage that a lifetime scope reclaims must not be named afterwards, so `sg::raw_buffer` carries explicit
expiry state, independent of how it was allocated:

- `is_expired()` / `is_valid()` — public: whether the buffer still names live storage.
- `expire()` — release the storage now (deferred until no longer in flight), even while handles remain.

A **transient** buffer is auto-expired when its epoch advances (its bump storage is about to be reused).
A **persistent** buffer can be expired **explicitly** to free memory early without hunting down every
`shared_ptr`.
Using an expired buffer in a transfer or a binding is a hard error (asserted). A transient
`binding_group` has the analogous rule — binding one past its epoch asserts, since the ring may have
recycled its descriptor slots.

## Status

Both **dedicated** and **placed** backing work for buffers on dx12, vulkan and metal, and for textures on dx12, vulkan and metal.
`ctx.persistent.create_memory_heap(size, contents)` mints a heap of buffers or of textures.
A placement's `allocation_info` routes the create through `CreatePlacedResource`, `vkBind*Memory` or the metal heap.
The resource holds a handle to its heap, so the heap outlives the placement.
`ctx.transient` bump-suballocates its buffers and textures, and exposes per-epoch binding groups.

## See also

- [allocation_info.hh](../../src/shaped-graphics/memory/allocation_info.hh) — the value type and `lifetime_scope`.
- [memory_heap.hh](../../src/shaped-graphics/memory/memory_heap.hh) — the heap factory and `memory_requirements`.
- [epochs](epochs.md) — the epoch retirement that bounds a transient resource's lifetime.
