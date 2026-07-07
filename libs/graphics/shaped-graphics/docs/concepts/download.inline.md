# Concept: inline download

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [epochs](epochs.md), [threading](threading.md), and the sibling [inline upload](upload.inline.md).

## What inline download is

**Inline download** reads GPU buffer bytes back to the host through a persistently-mapped
**READBACK-heap ring buffer** on the direct queue. `cmd.download.bytes_from_buffer` records a
`CopyBufferRegion` from the source into a reserved ring window **inline** in the recording list, and
returns a [`bytes_future`](../../src/shaped-graphics/bytes_future.hh) immediately. The actual bytes
arrive **asynchronously**: the GPU writes the readback window when it executes the list, and only then
can the CPU copy them into the caller's destination.

Because readback is inherently async and must not stall the recording thread, the CPU-side copy is
performed by a dedicated **`cc::threaded_actor`** (`sg-dx12-download`), not by the caller. This lets a
download complete **without advancing the epoch** — the recording thread records and moves on; the
future becomes ready when the actor has moved the bytes.

## The lifecycle of one download

1. **Record** (`download_buffer`, caller thread): reserve a ring window, record the `CopyBufferRegion`
   into the list, and append a **token-less** `dx12_download_copy_job` to the list. The future's
   destination is kept alive by a `pin` (a `weak_ptr` on the actor side).
2. **Submit** (`enqueue_submitted`, under the submission lock): stamp every job with the list's
   `submission_token`, mark its waiter *submitted*, and enqueue the jobs on the actor **in submission
   order**.
3. **Drain** (actor thread): for each job, block on the submission fence until the recording list has
   run, memcpy the readback bytes into the destination **if the pin is still alive**, mark the waiter
   ready, and release the job's hold on its epoch (see reclaim below).

Blocking rules follow from this. The completion-guaranteeing call is **`ctx.wait_for(future)`** (the
future itself carries only the non-blocking `is_ready()` / `try_get_bytes()` polls — a blocking wait is
a context-level effect, kept off the future). It refuses to block until the list is *submitted*
(otherwise it would stall the very thread that must submit) and returns `nullopt` for an unsubmitted or
cancelled download; the actor's per-job fence wait is what makes the readback bytes valid before the
memcpy. A future is waitable as soon as its list is submitted — **before** its epoch ends, no
`advance_epoch` required.

**What epoch waits do not guarantee.** `advance_epoch(...)` and `advance_epoch_and_wait_for_idle()` wait
on the **GPU epoch fence** only. Reaching idle means every readback `CopyBufferRegion` has finished on
the GPU and the ring bytes are valid — but the actor thread may not yet have been scheduled to run the
CPU memcpy and call `mark_ready()`. So `future.is_ready()` can be transiently **false** right after idle
returns (a thread-scheduling race, not a bug). Only `ctx.wait_for(future)` — which blocks on the
future's own waiter — guarantees the bytes have landed in the caller's destination. (The convergence in
the next section is about a *fully-drained epoch's counter reaching zero*, which is driven by the actor
calling `on_copy_done`, i.e. the actor's progress, not by reaching idle.)

## Why reclaim is epoch-granular (the load-bearing decision)

The subtle part is **when a ring window may be reused**. A window holds GPU-written readback bytes that
the actor must still copy out; reusing it early corrupts an in-flight download.

The tempting design — advance a free watermark to each job's end as the actor finishes it, in
submission order — is **wrong** under concurrent recording:

> Multiple command lists record **in parallel**, so their reservations interleave. Reservation order
> (which window sits where in the ring) is **allocation order**; the actor drains in **submission
> order**. These need not match. If list A reserves `[0,100)` and list B reserves `[100,200)`, but B is
> submitted first, the actor copies B first — and a per-submission watermark would jump past `[0,200)`,
> freeing A's window while A may not even be submitted yet. A later reservation reuses `[0,100)` and
> overwrites data A's GPU copy is still about to read/write. Silent corruption.

So the ring can only safely reclaim on **epoch boundaries**, where the "all downloads that reserved
into this span are accounted for" question has a clean answer. The mechanism:

- Each **epoch carries an outstanding-copy counter**. A reservation increments the *open* epoch's
  counter; the actor decrements it after draining a job (a dropped list decrements it too — see below).
- At **advance**, `on_epoch_advance(closed)` snapshots the ring cursor as the closing epoch's boundary
  and hands off its counter into an `epoch_checkpoint` FIFO; a fresh counter starts for the new epoch.
- **Reclaim** walks the FIFO from the front: a checkpoint's span frees only once its counter reaches
  **zero** — i.e. every download that reserved into that epoch has drained. A still-busy epoch blocks
  reclaim of everything reserved after it (FIFO order). This is the free watermark `reserve` waits on.

The actor draining a job (and `advance`, and `discard`) each trigger a reclaim pass. Note the actor
waits on the **submission fence** before copying, so a fully-drained epoch implies both its GPU
readback copies *and* its CPU memcopies are done — one counter subsumes both hazards, which is why the
download system needs **no** `process_completed_epochs` (GPU-retire) hook, only the advance hook.

**Allocation-order copies are still an improvement:** the actor copies in submission order rather than
forcing a global serialization, so interleaved lists pipeline; only *space reclaim* is coarsened to
epoch granularity, not the copies themselves.

As with upload, a single epoch whose inline downloads exceed the ring is a hard budget error (there is
no earlier epoch to wait on) — the documented v1 limitation.

## Dropping a list cancels its downloads

Dropping a recording list (`drop_command_list`) is distinct from dropping the *future*:

- **Dropping the future** (letting the `bytes_future` die) expires the pin. The list was still
  submitted, so the actor runs the job, sees the dead pin, and **skips the memcpy** but still counts
  the drain — space reclaims normally. This is the caller's way to cancel a download it no longer wants.
- **Dropping the list** (`discard_unsubmitted`) means the recorded copies will **never run**. Those
  futures can never complete, so each is explicitly **cancelled** (`mark_cancelled`) — `wait()` then
  fails instead of blocking forever, and `try_get_bytes` stays empty. The job's epoch-copy count is
  released so the epoch can still reach zero and reclaim. The reserved bytes are **not** freed
  individually — they sit inside the open epoch's span and are reclaimed with it at the next advance.

## Where the pieces live

- [`dx12_download_inline.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_inline.hh)
  / [`.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_inline.cc) — the ring,
  the actor, the per-epoch counters, and reclaim. The system **creates and maps its own READBACK heap**
  and starts the actor in `initialize` (colocated), off the context's device.
- [`dx12_resource_download.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_resource_download.hh)
  — the per-resource readback recorder + its **deferred CPU copy** (`dx12_buffer_download`), the closure
  the actor runs once the GPU copy has completed.
- [`bytes_future.hh`](../../src/shaped-graphics/bytes_future.hh) — the future/waiter the caller polls
  (`is_ready` / `try_get_bytes`) or waits on via `ctx.wait_for(future)`; `dx12_download_waiter` adds the
  *submitted* and *cancelled* gates.
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

## What's implemented today vs deferred

**Today:** inline **buffer** download over the READBACK ring, the actor, epoch-granular reclaim,
drop-to-cancel for both the future and the list.

**Deferred:** inline **texture** readback (row-unpadding in the deferred copy, chunked across several
jobs); splitting a window across the ring seam instead of wasting the tail on a would-be wrap (the
current v1 shortcut, mirroring [inline upload](upload.inline.md)); a fallback path when a single epoch's
downloads exceed the ring; and the finer split GPU/CPU watermarks noted in [epochs.md](epochs.md) if
profiling shows the single-counter coarsening costs pipelining.

## See also

- [inline upload](upload.inline.md) — the CPU→GPU mirror; its reclaim is fence-gated, not actor-driven.
- [epochs](epochs.md) — the epoch/submission timelines this builds on.
- [threading](threading.md) — why recording is concurrency-safe (and thus why reclaim must be
  epoch-granular).
- [cheat-sheet](../../cheat-sheet.md) — the download API at a glance.
