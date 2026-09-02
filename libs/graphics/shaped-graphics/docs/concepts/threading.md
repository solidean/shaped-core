# Concept: thread model

## What the thread model is

A backend declares its threading guarantees through [`sg::thread_model`](../../src/shaped-graphics/types.hh), reported by [`ctx.threading()`](context.md).
A caller reads it to know which context operations may run concurrently and which it must serialize itself.

The model is deliberately coarse today and expected to gain nuance — whether *concurrent command-list recording* is allowed, or per-queue guarantees.
Treat it as a small, growing capability tag, not a fixed contract.

```cpp
enum class thread_model
{
    single_threaded, // every context operation must be externally synchronized to one thread at a time
    multi_threaded,  // resource / command-list ops are concurrency-safe; epoch management + shutdown are not
};
```

## What each value promises

**`single_threaded`** — the caller must ensure no two context operations overlap.
A backend picks this when its underlying API or its own bookkeeping is not safe to touch from several threads at once.

**`multi_threaded`** — split into two tiers:

- **Concurrency-safe**, callable from several threads at once:
  - resource and command-list operations — `create_command_list`, `create_raw_buffer`, `submit_command_list`, `drop_command_list`, and a resource's refcount reaching zero;
  - the epoch waits and retire — `wait_for_epoch`, `wait_for_next_inflight_epoch`, `process_completed_epochs`;
    these are internally synchronized because they double as ring back-pressure invoked from within concurrent recording;
  - `wait_for(future)`, which touches only the future's own waiter and no context state.
- **Externally synchronized:** advancing (`advance_epoch`, `advance_epoch_and_wait_for_idle`) and **`shutdown`**.
  The caller must guarantee none of these overlaps any other context operation.
  Advancing closes an epoch and rewrites the shared in-flight state, including the current-epoch counter every other op reads, so fencing it off is the caller's job.
  That is also why advancing is a deliberate, rationed operation (see [epochs](epochs.md)).

A command list is still **single-threaded per instance** regardless of the model: one thread records it, then submits or drops it once, in the epoch it was opened in.
But **several command lists may record concurrently**, even against the same resource.
Each takes an access-tracking slot that keys its private per-resource state, so their recording shares no mutable state.
See [barriers](barriers.md) for the slot model and the entry barrier each submit prepends.

## Builds without threads

Where `CC_HAS_THREADS == 0` — WebAssembly, or any build configured `-DSC_THREADS=OFF` — nothing about the API changes.
The transfer systems still hand their copies to a [`cc::threaded_actor`](../../../../base/clean-core/src/clean-core/thread/threaded_actor.hh).
The actor simply runs on whoever sweeps it instead of on a thread of its own.

**sg owns no pumping of its own.**
An unthreaded actor registers itself with clean-core's [pump registry](../../../../base/clean-core/src/clean-core/thread/thread_pump.hh).
Every blocking wait — `cc::async_blocking_get`, a frame loop, one of the waits below — sweeps that registry rather than draining the actors it happens to know about.
`cc::thread_pump_all()` is the whole entry point, and it costs one atomic load where every actor has a thread of its own.

sg used to carry `sg::context::pump()` and a per-backend `on_pump()` for this, and the reason they are gone is that they could only ever drain what *this context* could name.
A wait below sg, or beside it, saw none of them: the deadlock that produced the registry was `cc::async_blocking_get` sleeping on a store it had no way to reach.

The waits that need the sweep are

- `wait_for(future)` and `wait_for_ticks` / `wait_for_seconds` — the readback actor delivers the bytes.
- `wait_for_epoch`, and so also `wait_for_next_inflight_epoch` and `advance_epoch`'s throttle — *not* just a GPU wait.
  A submitted list can be parked on a resource's async-upload timeline, which the copy actor signals, so without the sweep the GPU never reaches the epoch fence.
- the inline-download ring's back-pressure and its drain-to-idle — only the actor frees ring space and decrements the outstanding count.

The last two are the traps: they look like waits on the GPU or on an atomic, not on an actor.
What the registry leaves as an obligation sits on the ACTORS rather than on the waits.
**A handler must not block on progress another registration has to make**, because unthreaded it holds the only thread there is.
The inline-download actor is the live example.
It sweeps until its submission completes and only then falls through to the fence wait, because that submission can be queued behind an upload the copy actor has not run yet.

## Backends today

- **dx12** — `multi_threaded`.
  `create` / `submit` / `drop` are thread-safe: the open-command-list counter is atomic, and the command-allocator pool is mutex-guarded.
  The completion token is assigned together with the queue submit and fence signal under one lock, so token order equals signal order.
  `advance_epoch` and `shutdown` are externally synchronized.
- **vulkan** — `multi_threaded`, mirroring dx12.
  The open-command-list counter is atomic and the command-pool set is mutex-guarded.
  The completion token is assigned together with the `vkQueueSubmit` and timeline-semaphore signal under one lock, so token order equals signal order.
  `advance_epoch` and `shutdown` are externally synchronized.

## See also

- [context](context.md) — the operations this classifies, and which scope each one lives on.
- [types.hh](../../src/shaped-graphics/types.hh) — the `thread_model` enum.
- [epochs](epochs.md) — why epoch management is the externally-synchronized half.
