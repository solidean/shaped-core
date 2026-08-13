# Concept: epochs

## What an epoch is

An **epoch** is a coarse, frame-level lifetime token that doubles as a GPU synchronization point.
It is a monotonically increasing 64-bit counter ([`sg::epoch`](../../src/shaped-graphics/fwd.hh)), typically advanced once per rendered frame.
All GPU work recorded between two "advance" calls belongs to the same epoch.

The token is also a **timeline value**: the backend owns one **epoch fence** on its main (direct) queue, and the whole design rests on a single invariant —

> the direct queue signals the epoch fence with value `N` at the **end** of epoch `N`'s recorded work.
> So once the fence's completed value reaches `N`, every GPU operation from epoch `N` has finished, and everything epoch `N` owns is safe to reclaim.

Because both the counter and the fence are monotonic, "has epoch `N` finished?" is a single integer compare.
That is the point: lifetime questions for thousands of resources collapse into one fence read per epoch instead of one fence per resource.

Alongside it, [`sg::submission_token`](../../src/shaped-graphics/fwd.hh) is a **finer-grained** per-command-list value on a second direct-queue fence.
It answers "is *this one* list done?" rather than "is the whole frame done?".
Both are `uint64` timeline values on the same queue; the epoch fence drives reclamation, the submission fence is a convenience layer beside it.

## Why epochs exist

The GPU runs **behind** the CPU.
When CPU code stops referencing a buffer, the GPU may still be reading it for work submitted a frame or two ago.
Freeing that GPU memory — or resetting a command allocator, or recycling a descriptor slot — while the GPU might still touch it is a use-after-free on the device.

Reference counting alone cannot catch this: a refcount hitting zero says the *CPU* is done, not that *in-flight GPU* work is done.
Epochs supply the second gate cheaply by **batching**: everything that became garbage during a frame is grouped into one bucket keyed by an epoch value.
The whole bucket is reclaimed when that epoch's one fence signals.

This is exactly what makes submitting a command list safe: its allocator is held until the epoch retires, rather than reset while the GPU may still be draining the list.
Epochs also give sg a natural place to throttle how far the CPU runs ahead of the GPU.

## Only the concept is shared; the machinery is per-backend

What sg fixes is the vocabulary: the `epoch` / `submission_token` types and the epoch contract on [`sg::context`](context.md).
That is `current_epoch`, `advance_epoch`, `process_completed_epochs`, the `wait_for_*` family, and `completed_epoch`.
**How** a backend realizes them is its own business.
A backend may even uphold the contract **without** tracking real in-flight epochs.
An opengl backend, whose driver already manages resource lifetimes, could validate the contract against the counter alone.
That is sg's "duplicate across backends rather than abstract" stance — the shared piece is the vocabulary, never a cross-backend implementation.

The **dx12** backend is the reference realization (see [dx12_epoch.hh](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_epoch.hh) and its `.cc`).
The **vulkan** backend realizes the same design on a pair of timeline semaphores (see [vulkan_epoch.cc](../../backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_epoch.cc)).
Metal would map it onto shared events.

## Lifecycle: advance and retire

**Advance** (`advance_epoch`) closes the current epoch and opens the next:

1. Require that every command list opened this epoch has been submitted or dropped — **command lists cannot span epochs**.
   This is what makes per-epoch allocator recycling sound.
2. Increment the counter.
3. Signal the epoch fence with the *old* value on the direct queue (the core invariant above).
4. Package everything the old epoch owns — its command allocators and its expiring resources — into a per-epoch payload and push it onto the in-flight FIFO.
5. Optionally **throttle**: `allowed_in_flight` bounds how many epochs may remain in flight.
   `nullopt` never waits, `0` fully drains the GPU, and `N` keeps at most N — a windowed renderer typically passes its swapchain back-buffer count.
   `advance_epoch_and_wait_for_idle()` is the spelled-out `advance(0)`, named in full so an advance is never hidden behind a "wait" call.

Idle (and any `wait_for_*`) waits on the **GPU epoch fence** only.
It does **not** guarantee an inline **download** future is delivered: the readback CPU copy runs on a separate actor thread the epoch machinery does not drain.
So `future.is_ready()` can briefly lag idle, and `ctx.wait_for(future)` is the completion guarantee — see [inline download](download.inline.md).

**Retire** (`process_completed_epochs`) reclaims what the GPU has finished.
Read the fence once, drain every in-flight epoch whose value is `<= completed` (oldest first), and for each reclaim its payload — allocators back to the pool, expiring resources freed.
Retire is safe to call at any time.
`wait_for_epoch` and `wait_for_next_inflight_epoch` block on the fence and then retire, the latter being the standard back-pressure primitive when a pool is exhausted.
Neither `wait_for_*` advances the epoch — advancing is a deliberate, rationed operation kept distinct from waiting.

## Deferred deletion and finalizers

A persistent resource is lifetime-tracked by epoch:

> when a resource's refcount drops to zero, its GPU handle is **staged for deletion in the current epoch** and actually freed only once that epoch is no longer in flight.

So a resource's destructor does **not** free the GPU handle directly.
It hands the handle (and any **finalizers**) to the context's deferred-deletion staging area, which the next advance folds into the closing epoch's payload.
On retire the handle is nulled **first**, then finalizers run — and crucially **outside** the in-flight lock, because they may be slow or re-entrant and run on an unspecified thread.

**Finalizers** ([`buffer::add_finalizer`](../../src/shaped-graphics/resource/raw_buffer.hh)) are callbacks that run once a resource's GPU handle is released *and* it is no longer in flight.
They are the feedback point for reclaiming externally-owned backing memory.
That is the mechanism enabling **placed resources on custom allocators**, where the allocator needs a definite "the GPU is truly done with this" signal before recycling the memory.

## Load-bearing invariants

Preserve these; the rest is tuning:

1. **One monotonic epoch fence on the main queue, signaled with the epoch value at the end of that epoch's work.** (Vulkan: a timeline semaphore; Metal: a shared event.)
2. **Command lists cannot span epochs** — enforced per list (submitted or dropped in the epoch opened in) and in aggregate (no open lists at advance).
3. **FIFO retire order** — oldest epoch first, drained under one lock so per-epoch cleanup happens once and in order.
4. **Deferred deletion, not immediate free** — refcount→0 stages; the GPU free happens on retire.
5. **Null handles before running finalizers**, and run finalizers *outside* the in-flight lock.
6. **Throttle pipelining depth at advance** — default it to the swapchain back-buffer count for a windowed renderer.

## What's implemented today vs deferred

**Today (dx12 and vulkan):** the epoch counter and both direct-queue timelines (dx12 fences / vulkan timeline semaphores), the in-flight FIFO of per-epoch payloads, and advance/retire.
Plus the `allowed_in_flight` throttle, deferred deletion of buffers with finalizers, and per-epoch command-allocator (dx12) / command-pool (vulkan) recycling.
dx12 additionally holds a resource back past its epoch while an async upload to it is still in flight — a second gate on the copy fence, see [async upload](upload.async.md).

**Deferred** (see [TODO.md](../TODO.md)): the split GPU/CPU download watermarks for readback, and vulkan's async copy queue with its per-resource pending syncs.
Also placed transient *textures*: one works today, but as a dedicated allocation auto-expired at the next epoch rather than suballocated from the transient bump heap, which is buffers-only.
The in-flight FIFO still uses a `cc::vector` drained from the front; moving it to `cc::ringbuffer`, which now exists, is pending.

## See also

- [context](context.md) — the type the epoch contract sits on, and where every other scope hangs off it.
- [dx12_epoch.hh](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_epoch.hh) / [.cc](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_epoch.cc) — the reference realization.
- [cheat-sheet](../../cheat-sheet.md) — the epoch API at a glance.
