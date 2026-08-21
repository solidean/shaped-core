# Concept: inline download

## What inline download is

**Inline download** reads GPU bytes back to the host through a persistently-mapped **READBACK ring buffer** on the direct queue.
`cmd.download.bytes_from_buffer` records the readback copy into a reserved ring window **inline** in the recording list.
It returns a [`bytes_future`](../../src/shaped-graphics/bytes_future.hh) immediately.
The bytes arrive **asynchronously**: the GPU writes the readback window when it executes the list, and only then can the CPU copy them into the caller's destination.

That CPU-side copy runs on a dedicated **`cc::threaded_actor`**, so the recording thread records and moves on.
A download therefore completes **without advancing the epoch** — the future becomes ready once the actor has moved the bytes.
In a build without threads the actor owns no thread and runs on whoever pumps it, which may be the recording thread; see [threading](threading.md).

## The lifecycle of one download

1. **Record**, on the caller thread: reserve a ring window, record the readback copy into the list, and append a **token-less** copy job to it.
   The future's destination is kept alive by a `pin` — a `weak_ptr` on the actor side.
2. **Submit**, under the submission lock: stamp every job with the list's `submission_token`, open its `bytes_wait_gate`, and enqueue the jobs on the actor **in submission order**.
3. **Drain**, on the actor thread: block on the submission fence until the recording list has run, then memcpy the readback bytes into the destination **if the pin is still alive**.
   Then settle the future's completion async and release the job's hold on its epoch.

The completion-guaranteeing call is **`ctx.wait_for(future)`**.
The future itself carries only the non-blocking `is_ready()` / `try_get_bytes()` polls, since a blocking wait is a context-level effect and is kept off the future.
`wait_for` refuses to block until the list is *submitted* — blocking earlier would stall the very thread that must submit — and returns `nullopt` for an unsubmitted or cancelled download.
A future is waitable as soon as its list is submitted, **before** its epoch ends, with no `advance_epoch` required.

**What epoch waits do not guarantee.**
`advance_epoch(...)` and `advance_epoch_and_wait_for_idle()` wait on the **GPU epoch fence** only.
Reaching idle means every readback copy has finished on the GPU and the ring bytes are valid, but the actor thread may not yet have been scheduled to run the CPU memcpy.
So `future.is_ready()` can be transiently **false** right after idle returns — a scheduling race, not a bug.
Only `ctx.wait_for(future)`, which blocks on the future's own completion node, guarantees the bytes have landed in the caller's destination.

## Why reclaim is epoch-granular (the load-bearing decision)

The subtle part is **when a ring window may be reused**.
A window holds GPU-written readback bytes the actor must still copy out, so reusing it early corrupts an in-flight download.

Advancing a free watermark to each job's end as the actor finishes it, in submission order, is the tempting design — and it is **wrong** under concurrent recording:

> Multiple command lists record **in parallel**, so their reservations interleave.
> Reservation order — which window sits where in the ring — is **allocation order**, while the actor drains in **submission order**, and the two need not match.
> Say list A reserves `[0,100)` and list B reserves `[100,200)`, but B is submitted first.
> The actor copies B first, and a per-submission watermark would jump past `[0,200)`, freeing A's window while A may not even be submitted yet.
> A later reservation then reuses `[0,100)` and overwrites data A's GPU copy is still about to read.
> Silent corruption.

So the ring can only safely reclaim on **epoch boundaries**, where "is every download that reserved into this span accounted for?" has a clean answer.
The mechanism:

  Each pushed copy job increments the *open* epoch's counter, and the actor decrements it after draining that job — as does a dropped list, see below.
  The count is per job, not per reservation, so a read split across several windows counts once per window that actually copies.
  A reservation increments the *open* epoch's counter, and the actor decrements it after draining a job — as does a dropped list, see below.
- At **advance**, the ring cursor is snapshotted as the closing epoch's boundary and its counter handed into a checkpoint FIFO, with a fresh counter starting for the new epoch.
- **Reclaim** walks that FIFO from the front: a checkpoint's span frees only once its counter reaches **zero**, meaning every download that reserved into that epoch has drained.
  A still-busy epoch blocks reclaim of everything reserved after it.
  This is the free watermark a reservation waits on.

Draining a job triggers a reclaim pass, as do advance and discard.
The actor waits on the **submission fence** before copying, so a fully-drained epoch implies both its GPU readback copies *and* its CPU memcopies are done.
One counter subsumes both hazards, which is why the download system needs **no** GPU-retire hook, only the advance hook.

Only *space reclaim* is coarsened to epoch granularity, not the copies themselves.
The actor still copies in submission order rather than serializing globally, so interleaved lists pipeline.

As with upload, a single epoch whose inline downloads exceed the ring is a hard budget error — there is no earlier epoch left to wait on.

## Dropping a list cancels its downloads

Dropping a recording list (`drop_command_list`) is distinct from dropping the *future*:

- **Dropping the future** expires the pin.
  The list was still submitted, so the actor runs the job, sees the dead pin, **skips the memcpy** and still counts the drain — space reclaims normally.
  This is how a caller cancels a download it no longer wants.
- **Dropping the list** means the recorded copies will **never run**.
  Those copies can never run, so each future is explicitly **cancelled** — `push_error(cc::async_error::make_cancelled())` on its completion node.
  It then reads as settled, `try_get_bytes` stays empty, and `ctx.wait_for` fails instead of blocking forever.
  Saying it out loud is mandatory rather than tidy: a manual async node nobody ever pushes parks its dependents for the process's lifetime.
  The job's epoch-copy count is released, so the epoch can still reach zero and reclaim.
  The reserved bytes are **not** freed individually — they sit inside the open epoch's span and reclaim with it at the next advance.

## Where the pieces live

- [`dx12_download_inline.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_inline.hh)
  / [`.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_inline.cc)
  — the ring, the actor, the per-epoch counters, and reclaim.
  The system **creates and maps its own READBACK heap** and starts the actor in `initialize`, off the context's device.
- [`dx12_resource_download.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_resource_download.hh)
  — the per-resource readback recorder plus its **deferred CPU copy** (`dx12_buffer_download`), the closure the actor runs once the GPU copy has completed.
- [`bytes_future.hh`](../../src/shaped-graphics/bytes_future.hh)
  — the future the caller polls (`is_ready` / `try_get_bytes`), chains off (`completion()`), or waits on via `ctx.wait_for(future)`.
  Cancellation rides the completion node's error channel.
  The separate `sg::bytes_wait_gate` carries only the *submitted* question, since blocking before submit would stall the very thread that must submit.
- The advance hook is called from [`dx12_epoch.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_epoch.cc).

## Load-bearing invariants

Preserve these; the rest is tuning:

1. **The GPU copy is recorded inline**, but the CPU copy is **deferred to the actor** — a download never
   stalls the recording thread and never requires an epoch advance to complete.
2. **Ring space reclaims at epoch granularity**, gated on a per-epoch outstanding-copy counter reaching
   zero — never per submission, because concurrent recording divorces allocation order from submission
   order.
3. **The actor copies in submission order**, waiting on the submission fence before each memcpy.
4. **A dead pin cancels the copy** (drop-the-future); **a dropped list cancels the futures** and
   releases their epoch counts (drop-the-list). Neither leaks ring space.
5. **The blocking wait (`ctx.wait_for(future)`) cannot block before submission** — blocking a
   not-yet-submitted download would deadlock the submitting thread; it returns `nullopt` instead.

## Runtime resize

`ctx.download.set_budget(bytes)` records a pending readback-ring capacity, applied at the next `advance_epoch`.
The apply cannot simply wait on the epoch fence the way the [inline upload](upload.inline.md) ring does.
The actor's deferred copies read out of the *current* ring **after** GPU retire, so freeing it at the epoch fence would pull the memory out from under an in-flight memcpy.
So the apply first drains the in-flight epochs, bounding the GPU wait, then waits for the actor to finish every outstanding readback, and only then drops and rebuilds the ring.
The freshly-opened epoch has no downloads yet, so that outstanding count reaches zero.

## What's implemented today vs deferred

**Today:** inline **buffer** and **texture** download over the READBACK ring, the actor, epoch-granular reclaim, and drop-to-cancel for both the future and the list.
A read larger than the ring, or one straddling the seam, splits into several contiguous windows.
A texture region's chunks are each un-padded into the tightly-packed destination.

**Deferred:** a fallback path when a single epoch's downloads exceed the ring.
Also the finer split GPU/CPU watermarks noted in [epochs](epochs.md), should profiling show the single-counter coarsening costs pipelining.

## See also

- [inline upload](upload.inline.md) — the CPU→GPU mirror; its reclaim is fence-gated, not actor-driven.
- [async download](download.async.md) — the copy-queue, off-frame sibling (`ctx.download`) for bulk readback.
- [epochs](epochs.md) — the epoch/submission timelines this builds on.
- [threading](threading.md) — why recording is concurrency-safe (and thus why reclaim must be
  epoch-granular).
- [cheat-sheet](../../cheat-sheet.md) — the download API at a glance.
