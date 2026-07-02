# Concept: epochs

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)) and not the roadmap
> (that's [structure.md](../structure.md)). This is the first entry in `docs/concepts/`; more
> cross-cutting sg concepts will grow beside it.

## What an epoch is

An **epoch** is a coarse, frame-level lifetime token that doubles as a GPU synchronization point. It
is a monotonically increasing 64-bit counter ([`sg::epoch`](../../src/shaped-graphics/fwd.hh)),
typically advanced once per rendered frame. All GPU work recorded between two "advance" calls belongs
to the same epoch.

The token is also a **timeline value**: the backend owns one **epoch fence** on its main (direct)
queue, and the whole design rests on a single invariant —

> the direct queue signals the epoch fence with value `N` at the **end** of epoch `N`'s recorded
> work. So once the fence's completed value reaches `N`, every GPU operation from epoch `N` has
> finished, and everything epoch `N` owns is safe to reclaim.

Because both the counter and the fence are monotonic, "has epoch `N` finished?" is a single integer
compare. That is the point: lifetime questions for thousands of resources collapse into one fence
read per epoch instead of one fence per resource.

Alongside it, [`sg::submission_token`](../../src/shaped-graphics/fwd.hh) is a **finer-grained**
per-command-list value on a second direct-queue fence, for callers who need "is *this one* list
done?" rather than "is the whole frame done?". Both are just `uint64` timeline values on the same
queue; the epoch fence drives reclamation, the submission fence is a convenience layer beside it.

## Why epochs exist

The GPU runs **behind** the CPU. When CPU code stops referencing a buffer, the GPU may still be
reading it for work submitted a frame or two ago. Freeing that GPU memory — or resetting a command
allocator, or recycling a descriptor slot — while the GPU might still touch it is a use-after-free on
the device.

Reference counting alone can't catch this: a refcount hitting zero says the *CPU* is done, not that
*in-flight GPU* work is done. Epochs supply the second gate cheaply by **batching**: everything that
became garbage during a frame is grouped into one bucket keyed by an epoch value, and the whole
bucket is reclaimed when that epoch's one fence signals.

This is exactly what makes submitting a command list safe: its allocator is held until the epoch
retires rather than reset while the GPU may still be draining the list. Epochs also give sg a natural
place to throttle how far the CPU runs ahead of the GPU.

## Only the concept is shared; the machinery is per-backend

In sg, the **concept** is abstract — the `epoch` / `submission_token` types and the virtual contract
on [`sg::context`](../../src/shaped-graphics/context.hh) (`current_epoch`, `advance_epoch`,
`process_completed_epochs`, `wait_for_epoch`, …). **How** a backend realizes epochs is its own
business, and a backend may uphold the contract **without** tracking real in-flight epochs at all —
e.g. an opengl backend, whose driver already manages resource lifetimes, could just validate the
contract against the counter. This follows sg's "duplicate across backends rather than abstract"
stance: the shared piece is the vocabulary, not a cross-backend implementation.

The **dx12** backend is the reference realization (see
[dx12_epoch.hh](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_epoch.hh) /
[dx12_epoch.cc](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_epoch.cc)). The concepts
map cleanly onto Vulkan timeline semaphores or Metal shared events.

## Lifecycle: advance and retire

**Advance** (`advance_epoch`) closes the current epoch and opens the next:

1. Require that every command list opened this epoch has been submitted or dropped — **command lists
   cannot span epochs**. This is what makes per-epoch allocator recycling sound.
2. Increment the counter.
3. Signal the epoch fence with the *old* value on the direct queue (the core invariant above).
4. Package everything the old epoch owns — its command allocators and its expiring resources — into a
   per-epoch payload and push it onto the in-flight FIFO.
5. Optionally **throttle**: `allowed_in_flight` bounds how many epochs may remain in flight (`nullopt`
   = never wait; `0` = full GPU drain; `N` = keep at most N; a windowed renderer typically passes its
   swapchain back-buffer count). `advance_epoch_and_wait_for_idle()` is the spelled-out `advance(0)` —
   named in full so the advance is never hidden behind a "wait" call.

**Retire** (`process_completed_epochs`) reclaims what the GPU has finished: read the fence once, drain
every in-flight epoch whose value is `<= completed` (oldest first), and for each reclaim its payload —
reset allocators back to the pool, and free expiring resources. Retire is safe to call at any time;
`wait_for_epoch` and `wait_for_next_inflight_epoch` block on the fence and then retire (the latter is
the standard back-pressure primitive when a pool is exhausted). Neither `wait_for_*` advances the
epoch — advancing is a deliberate, rationed operation kept distinct from waiting.

## Deferred deletion and finalizers

A persistent resource is lifetime-tracked by epoch:

> when a resource's refcount drops to zero, its GPU handle is **staged for deletion in the current
> epoch** and actually freed only once that epoch is no longer in flight.

So a resource's destructor does **not** free the GPU handle directly; it hands the handle (and any
**finalizers**) to the context's deferred-deletion staging area, which the next advance folds into the
closing epoch's payload. On retire the handle is nulled **first**, then finalizers run — and
crucially **outside** the in-flight lock, because they may be slow or re-entrant and run on an
unspecified thread.

**Finalizers** ([`buffer::add_finalizer`](../../src/shaped-graphics/buffer.hh)) are callbacks that run
once a resource's GPU handle is released *and* it is no longer in flight. They are the feedback point
for reclaiming externally-owned backing memory — the mechanism that later enables **placed resources
on custom allocators**, where the allocator needs a definite "the GPU is truly done with this" signal
before recycling the memory.

## Load-bearing invariants

Preserve these; the rest is tuning:

1. **One monotonic epoch fence on the main queue, signaled with the epoch value at the end of that
   epoch's work.** (Vulkan: a timeline semaphore; Metal: a shared event.)
2. **Command lists cannot span epochs** — enforced per list (submitted/dropped in the epoch opened in)
   and in aggregate (no open lists at advance).
3. **FIFO retire order** — oldest epoch first, drained under one lock so per-epoch cleanup happens once
   and in order.
4. **Deferred deletion, not immediate free** — refcount→0 stages; the GPU free happens on retire.
5. **Null handles before running finalizers**, and run finalizers *outside* the in-flight lock.
6. **Throttle pipelining depth at advance** — default it to the swapchain back-buffer count for a
   windowed renderer.

## What's implemented today vs deferred

**Today (dx12):** the epoch counter + both direct-queue fences, the in-flight FIFO of per-epoch
payloads, advance/retire, the `allowed_in_flight` throttle, deferred deletion of buffers with
finalizers, and per-epoch command-allocator recycling. The vulkan backend stubs the contract.

**Deferred** (see [TODO.md](../TODO.md)): the async copy queue with pooled group fences and
per-resource pending syncs; transient resources (linear bump allocator + transient descriptor
ring-buffers); and the split GPU/CPU download watermarks for readback. The in-flight FIFO uses a
`cc::vector` drained from the front until `cc::ringbuffer` is implemented.

## See also

- [dx12_epoch.hh](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_epoch.hh) /
  [dx12_epoch.cc](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_epoch.cc) — the reference
  realization (payload structs, advance/retire, waits).
- [context.hh](../../src/shaped-graphics/context.hh) — the epoch contract on `sg::context`.
- [cheat-sheet](../../cheat-sheet.md) — the epoch API at a glance.
