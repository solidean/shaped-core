# Concept: streaming transfers

## What streaming is

**Streaming** is the deliberately weaker sibling of [async upload](upload.async.md) and [async download](download.async.md), reached as `ctx.stream`.

Those two carry a **strong scheduling guarantee**: sync is automatic in both directions, so the next command list touching the resource waits on the copy.
That is right for must-be-there data, and wrong for bulk asset traffic — there the same automatic wait turns "slow" into "stall", and uploading a gigabyte costs a frame hitch.

Streaming rides the same copy queue, the same windows and the same actor.
It trades the automatic synchronization for a **handle**: dynamic priority, progress, cancellation, and a `cc::async` completion node other work can depend on.

It is not a separate system.
There is one job queue, one window packer and one copy queue per direction; a streaming transfer is a **job flavor**, and only the public API is separate.

## The contract (the load-bearing decision)

> The streamed extent is **yours alone** between the call and the moment the handle reports complete.
> Any command list touching it must be **submitted** after you observed that.
> Everything outside the extent is unaffected.

Three things about that sentence are doing work.

**"Submitted", not "recorded."**
A list recorded during the transfer and submitted after it settles is perfectly legal.
That is also why the dynamic check, when it lands, belongs at submit: a record-time check would fire on correct code, and a check people turn off is worse than no check.

**"Reports complete" means the copy has run**, not that the last chunk was recorded.
For an upload those are different moments, and the difference is a half-written buffer.
So a finished streaming upload waits on the copy fence before it settles, which async upload never needs because its completion is a GPU-side wait rather than a CPU signal.

**"The extent"** is what `stream_scope` names, and it is checked.
`resource` claims the whole thing, `subresource` one mip or slice, `region` a byte range or a box inside one subresource.

## Why the scope is declared rather than inferred

Both of the things a backend may need — D3D12's `ALLOW_SIMULTANEOUS_ACCESS` and Vulkan's sharing mode — are **creation-time** properties.
A resource that did not record the intent when it was created cannot be streamed into narrowly later, at all.
So the intent is a usage flag, and the per-call `stream_scope` is checked against it.

| declared usage | dx12 buffer | dx12 texture | vulkan (both) | metal / webgpu |
|---|---|---|---|---|
| *(none)* | — | — | — | — |
| `allow_subresource_stream` | n/a | — | `CONCURRENT` sharing | — |
| `allow_region_stream` | — | `ALLOW_SIMULTANEOUS_ACCESS` | `CONCURRENT` sharing | — |

Buffers have no subresources, so they carry only `allow_region_stream` — the asymmetry is the model being honest rather than an oversight.

**`stream_scope::resource` is free on every backend**, which is what protects the common case: streaming into something nothing else is looking at yet.
The narrower scopes are not free.
`ALLOW_SIMULTANEOUS_ACCESS` rules out depth/stencil and MSAA and can disable metadata compression, so it follows `allow_region_stream` only.
Never `allow_subresource_stream`, which dx12 gets for nothing because resource state is already tracked per subresource.

Vulkan is the strict backend here and is not implemented yet; the sharing-mode requirement is recorded at the creation sites.

## How windows are shared

Sharing is **per window, not within one**.
A rolling deficit picks which tier fills the *current* window first, and the other fills whatever is left.

Splitting each window by ratio reads as the more direct design and does not survive textures.
A placed footprint has a minimum viable slice, so a reserved fraction too small for one aligned row is simply wasted.
Whole windows have no such failure mode.

The deficit counts the bytes a window **actually moved**, so a stream-primary window that only manages a tenth of a window does not burn the whole share.
It is bounded to one window's worth in each direction, so a long idle stretch cannot bank credit it would then spend all at once.

The ratio is a **floor on throughput, not a cap**: with no async work pending, streaming gets everything.
`ctx.stream.set_upload_ratio` / `set_download_ratio` tune it per direction, since upload and download own independent copy queues.

Selection within the streaming tier is highest effective priority first, ties broken by submission order.
**First-in-first-out within a tier, never round-robin.**
A stream is worth something only once it *completes*: ten half-finished textures are worth nothing, five finished ones are worth five.
Aging (`priority + factor * seconds_waiting`) exists but defaults to off.
A low-priority transfer never running while higher-priority work exists is usually what the caller meant.

All of this is [`sg::impl::transfer_scheduler`](../../src/shaped-graphics/transfer/impl/transfer_scheduler.hh), which knows no backend type and is tested without a GPU.
That is where nearly all of the system's risk lives.

## Load-bearing invariants

Preserve these; the rest is tuning:

1. **A streaming transfer never stamps the forward reader value.**
   That stamp is what makes a later command list wait, and not paying it is the entire point of the tier.
   The storage still has to outlive the copy, so the lifetime gate reads a *separate* per-resource stamp, and deferred deletion waits on the max of the two.
2. **Every teardown path settles the completion node.**
   Cancellation, a dropped handle, a dropped destination, context shutdown — all of them push `cc::async_error::make_cancelled()`.
   A manual async node nobody pushes parks its dependents for the process's lifetime, so silence is the one unacceptable outcome.
3. **An upload settles only after its copy has run.**
   See the contract above; settling at record time is the bug that looks like it works.
4. **Cancellation bounds future work rather than undoing past work.**
   It is a flag, not queue surgery: the job stops being picked and is reaped when the actor next looks, and chunks already recorded still run.

## What the handle offers

`set_priority` and `cancel` are relaxed atomic stores from any thread — no message, no lock — read by the actor when it next picks, which is once per window.

`promote_to_async()` is **additive**: the transfer keeps its handle, its progress and its completion, and *additionally* gains the automatic synchronization.
It is what makes a low-priority stream a safe prewarm — guessing wrong about what will be needed is recoverable rather than fatal.
Lists recorded before the call are unaffected, which is the same rule the contract already states.

There is deliberately **no `detach`**.
`ctx.upload` is the fire-and-forget path — no handle, no observer, automatic synchronization — and streaming always has an observer.
Two modes, no overlap.
Dropping a streaming handle therefore means cancel rather than "carry on unwatched".

`progress()` reports bytes staged plus an optional total hint.
It is a hint because a source need not know its own size up front.
It is never the thing to build a completion test on — the handle and its completion node are.

## Where the bytes come from

The plain form takes one pinned blob, which is the "this upload is slow, do not hitch the frame" case and needs
nothing more.
A transfer that must not hold its whole payload resident wants the other form: `ctx.stream.from_source_to_buffer`
and `from_source_to_texture` take a [`stream_source`](../../src/shaped-graphics/transfer/stream_source.hh), a lazy
sequence of chunks the copy actor pulls from as windows open.

The resident form is not a separate path — it builds a source of one always-ready chunk, so there is one
implementation underneath and no second thing to keep correct.

Two properties carry the design.

**A poll must not block.**
It runs on the copy actor thread, which stages every other transfer in the system, so a source that waits on a file
read stalls all of them.
`not_yet` is the answer for "my data is not back yet": the transfer is passed over and the window is filled with
other work, costing it a window rather than costing the system a thread.

**A poll has four answers, not two.**
`not_yet` and `done` are genuinely different — conflating them either stalls a finished transfer or completes an
unfinished one — and `failed` is the only way out for a source that cannot deliver what it promised.
Without it a stalled transfer would sit in the queue forever, and everything chained onto its completion with it.

A stalled source resumes when its **waker** fires, which the system installs on admission.
Without it a stalled transfer would resume only when some other message happened to wake the actor: constantly in a
busy system, never in a quiet one.
The waker is safe to call from any thread at any point, including after the transfer ended and during shutdown.

Chunk order is unconstrained, since each chunk carries its own offset and the streaming contract makes the
destination unreadable until the handle settles.
For a texture the offsets are into the region's tightly-packed bytes and must fall on **row** boundaries — a row is
the smallest unit a texture copy can place, so a part-row chunk has nothing it could be copied into.

## Current simplifications (deferred)

Not invariants — v1 shortcuts:

- **The destination sink is the pinned buffer.**
  A custom sink would let a streamed download land in a file or a decoder without a full CPU-side copy.
- **No absolute bandwidth cap.**
  The ratio protects streaming from the rest of the system; a cap would protect the rest of the system from streaming.
  It wants profiles first.
- **No dynamic scope validation.**
  The static check against usage flags is always on.
  Catching a command list that actually touches a streamed extent needs a per-resource record checked at submit,
  and for `region` scope an interval set that is dev-only.
- **dx12 only**, like the async tier it rides on.

## See also

- [async upload](upload.async.md) / [async download](download.async.md) — the strong-guarantee tier, and the windows and fences streaming shares.
- [context](context.md) — the scope this hangs off.
- [epochs](epochs.md) — the deferred-deletion gate the streaming lifetime stamp feeds.
- [cheat-sheet](../../cheat-sheet.md) — the API at a glance.
