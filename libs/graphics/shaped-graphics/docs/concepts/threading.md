# Concept: thread model

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [epochs](epochs.md).

## What the thread model is

A backend declares its threading guarantees through [`sg::thread_model`](../../src/shaped-graphics/types.hh),
reported by `sg::context::threading()`. A caller reads it to know which context operations may run
concurrently and which it must serialize itself.

The model is deliberately coarse today and expected to gain nuance (for example, whether *concurrent
command-list recording* is allowed, or per-queue guarantees). Treat it as a small, growing capability
tag, not a fixed contract.

```cpp
enum class thread_model
{
    single_threaded, // every context operation must be externally synchronized to one thread at a time
    multi_threaded,  // resource / command-list ops are concurrency-safe; epoch management + shutdown are not
};
```

## What each value promises

**`single_threaded`** — the caller must ensure no two context operations overlap. A backend picks this
when its underlying API or its own bookkeeping is not safe to touch from several threads at once.

**`multi_threaded`** — split into two tiers:

- **Concurrency-safe:** resource and command-list operations — `create_command_list`,
  `create_buffer`, `submit_command_list`, `drop_command_list` (and a resource's refcount reaching
  zero, i.e. deferred deletion) — may be called from several threads at once.
- **Externally synchronized:** **epoch management** (`advance_epoch`,
  `advance_epoch_and_wait_for_idle`, `wait_for_epoch`, `wait_for_next_inflight_epoch`,
  `process_completed_epochs`) and **`shutdown`**. The caller must guarantee none of these overlaps any
  other context operation. Advancing is a frame-boundary decision that closes an epoch and rewrites
  the shared in-flight state, so it is the caller's job to fence it off — which is also why advancing
  is a deliberate, rationed operation (see [epochs](epochs.md)).

A command list is still **single-threaded per instance** regardless of the model: one thread records
it, then submits or drops it once, in the epoch it was opened in.

## Backends today

- **dx12** — `multi_threaded`. `create` / `submit` / `drop` are thread-safe: the open-command-list
  counter is atomic, the completion token is assigned together with the queue submit + fence signal
  under one lock (so token order equals signal order), and the command-allocator pool is
  mutex-guarded. `advance_epoch` and `shutdown` are externally synchronized.
- **vulkan** — `multi_threaded`, mirroring dx12: the open-command-list counter is atomic, the
  completion token is assigned together with the `vkQueueSubmit` + timeline-semaphore signal under one
  lock (so token order equals signal order), and the command-pool set is mutex-guarded. `advance_epoch`
  and `shutdown` are externally synchronized.

## See also

- [types.hh](../../src/shaped-graphics/types.hh) — the `thread_model` enum.
- [context.hh](../../src/shaped-graphics/context.hh) — `threading()` and the operations it classifies.
- [epochs](epochs.md) — why epoch management is the externally-synchronized half.
