# Concept: async download

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)), and backend-neutral (the
> per-backend realization is a section at the end). See also [epochs](epochs.md), [threading](threading.md),
> and the siblings [async upload](upload.async.md) and [inline download](download.inline.md).

## What async download is

**Async download** streams GPU→CPU buffer readback on a **dedicated transfer (copy) queue**, off the frame
path. `ctx.download.bytes_from_buffer` hands the read to a background
[`cc::threaded_actor`](threading.md) and returns a [`bytes_future`](../../src/shaped-graphics/bytes_future.hh)
immediately. The actor records a copy from the source into a persistently-mapped **readback** staging
buffer on the transfer queue, then memcpys the staged bytes into the caller's destination and marks the
future ready. It is the context-level mirror of the inline [`cmd.download`](download.inline.md):
**`cmd.download` = inline (recorded in a command list, delivered once that list runs); `ctx.download` =
async (transfer queue, decoupled from any list or epoch)**.

It is the right tool for **bulk readback** — asset baking, screenshots, GPU→CPU result streaming — that
must not stall the recording thread or burn main-queue time. For a small must-be-back-now readback tied to
a specific list's output, use [inline download](download.inline.md).

**Fire-and-return-a-future.** The call returns a `bytes_future`; the copy runs later, on a queue the caller
never sees. Block on it with `ctx.wait_for(future)`, or poll `future.is_ready()` / `future.try_get_bytes()`.
The returned `pinned_data` keeps the bytes alive on its own, so it stays valid past the future's lifetime.
A zero-size read yields an already-ready, empty future.

## Why sync is automatic in both directions (the load-bearing decision)

The read runs **later**, asynchronously, on a queue the caller never sees. For the CPU timeline
`submit → async download → submit` to just work — no manual barrier — the system needs sync in **both
directions**, each a **cross-queue GPU wait** (no CPU stall). It reuses the same per-resource stamps
[async upload](upload.async.md) established, with the roles mirrored:

**Forward — the read waits on the last writer.** A command list that wrote the buffer before the download
must finish before the read runs, or the read observes stale bytes.

- Each command list, at submit, **stamps every buffer it used with its submission token** (atomic max —
  the same stamp async upload's reverse wait uses).
- The async download reads that token when recorded and carries it; when the actor submits the window
  holding the read, it makes the **transfer queue wait on the main queue's submission fence** for that
  token first. One wait per window covers every buffer read in it.

**Forward — the read also waits on a pending async upload.** An async *upload* to the same buffer runs on a
**separate** transfer queue (see below), so it is not covered by the submission-fence wait. The download
reads the buffer's pending-async-upload value when recorded and carries it; the window holding the read
**waits on the upload completion fence** for that value before executing — a clean cross-queue GPU wait, so
`submit-upload → async-download` of the same buffer just works with no CPU stall.

**Reverse — a later writer waits on the read.** A command list that **writes** the buffer after the
download must not overwrite the bytes while the transfer queue is still reading them.

- The download reserves a **completion value** on a monotonic counter and **stamps it onto the buffer**
  (atomic max) before the actor runs, so any writer recorded afterwards sees a value to wait on.
- Every op that **writes** a buffer while recording folds that buffer's download value into the list's
  required wait — **only writes**, since two reads never conflict.
- At **submit**, the main queue is told to **wait on the transfer queue's completion fence** for that value
  before executing the list. The transfer queue signals that fence once the window holding the read has
  completed, so the write lands strictly after the read.

Both waits point strictly **backward in the CPU submission order**, so *per operation* the dependency
graph is acyclic — no deadlock. Multiple async downloads of the **same** buffer are independent reads; two
reads never conflict, so they need no ordering against each other.

**One scheduling rule keeps that acyclic at the window level.** Each forward wait (submission token *and*
upload-completion value) is issued once per *window*, on the max over its reads, hoisted ahead of the
window's execute. So a window must never *both* signal a completion `V` (that a later writer waits on) *and*
carry a forward wait that (transitively) depends on `V` — the hoisted wait would then sit ahead of the very
read whose signal it needs, closing a cycle. The actor enforces this exactly as async upload does: it
**closes the open window before staging a job** whose forward token *or* upload-completion value is still
pending once the window has already finished a read.

**Why upload and download own separate transfer queues.** A `Wait` on a GPU queue blocks *all* work behind
it in that queue's FIFO. If upload and download shared one queue, an upload window waiting on a direct-queue
token and a download window (queued behind it) that would release that token could **deadlock** — the
per-window acyclicity rule above only reasons about one actor's own windows, not the other actor's work
sharing the queue. Giving each system its own queue removes that cross-actor coupling: the only ordering
between them is the explicit upload-completion wait above, which points strictly backward. (On backends
where a second transfer queue is not guaranteed — see the vulkan note below — this needs a fallback.)

## Why a download completes with a CPU memcpy (and how windows drain)

Unlike async upload — where staging a copy is the last the actor does with a job — a download only finishes
once the **CPU memcpy** out of the readback staging buffer has run. That post-GPU step is what a download's
`bytes_future` becomes ready on.

The staging buffer is triple-buffered into fixed **windows** ([three](upload.async.md), same as upload), so
GPU read and CPU memcpy overlap. But because the memcpy must run *after* the window's GPU read, each
submitted window is kept **in flight** until it is **drained**: the actor waits on that window's staging
fence, memcpys its chunks into their destinations, and marks their waiters ready. It drains a window:

- **before reusing its slot** — a window's slot is reused three submissions later, and reuse must wait not
  only for the GPU read (staging fence) but for the CPU memcpy too, else it would overwrite bytes the
  memcpy still needs; and
- **for every remaining in-flight window when the inbox empties**, before the actor sleeps — a sleeping
  actor must leave nothing undrained, or a `bytes_future` would never become ready (no epoch advance forces
  it; only the actor's memcpy does).

During bulk streaming this pipelines to depth three (one window read by the GPU, one just submitted, one
filled by the CPU); when the inbox drains it flushes. A read **larger than one window** packs across
successive windows; the window holding the read's last byte carries the completion value and the waiter, and
because windows drain in order the earlier chunks are already copied by the time that waiter is marked ready.

Two fences on the transfer queue, both signaled per window:
- the **staging fence** — every window, gating window reuse (and thus drain ordering);
- the **completion fence** — signaled up to the *highest finished read value* in that window (skipped when
  a window carries only a mid-read chunk), which is what a later writer's reverse wait blocks on.

## Why the source is held strong (and the future's pin weak)

The job holds a **strong** handle to the source buffer for its whole lifetime, so the source storage stays
alive across the transfer-queue read. This is the one place async download is *simpler* than async upload:
upload holds a `weak_ptr` (so a dropped-target upload can be skipped) and needs a deferred-deletion **copy
gate** to keep storage alive past the strong lock; download's strong hold makes that gate unnecessary.

The **destination** is held **weak** — a `weak_ptr` on the future's pin. **Dropping the future cancels the
copy at the next opportunity:**

- If the pin has expired by the time the actor reaches the job (**stage time**), the read is skipped
  entirely — no `CopyBufferRegion`, no forward wait — so a large read to an abandoned future never touches
  the GPU. But the job's **completion value is still folded** into a window so the completion fence reaches
  it: a later writer stamped with that value must not hang. (This is exactly async upload's dropped-target
  rule: a cancelled job still signals its value; only the copy is skipped.)
- If the pin expires **after** the read was recorded (**drain time**), the memcpy is skipped; the staged
  bytes are simply not copied out.

## Load-bearing invariants

Preserve these; the rest is tuning:

1. **Sync is automatic in both directions.** Forward: the read waits on the last command list that wrote
   the buffer (submission token stamped at submit, transfer queue waits before the read). Reverse: a later
   command list that *writes* the buffer waits on the read (completion value stamped before enqueue, main
   queue waits at submit). Only writes fold in the reverse wait; waits point backward in submission order
   (acyclic).
2. **A download completes with a CPU memcpy on the actor**, deferred until the window's GPU read finishes —
   a download never stalls the recording thread and never needs an epoch advance to complete. Every window
   is drained before its slot is reused and before the actor sleeps.
3. **The source is held strong across the read** (no copy gate); the destination is held weak, so dropping
   the future cancels the copy — but a cancelled read still signals its reverse-sync completion value.

## Current simplifications (deferred)

Not invariants — v1 shortcuts:

- **Persistent buffers only** and **single-writer**: an async download of a buffer concurrently written
  by an in-flight list is the caller's hazard to avoid.
- **In-order reads (head-of-line blocking).** Reads run strictly in submission order on the download's own
  transfer queue, so a forward wait on a slow command list stalls all later async reads behind it — a
  deferred optimization, as with upload.
- **Coarser than per-buffer state**: the stamps are single monotonic values per buffer, a down-payment on
  the per-resource state-tracking layer landing separately, which should replace them.
- **Submits a partially-filled window early** (when the inbox drains) rather than claiming its unused
  tail for the next job — a low-latency choice, not a size limit. Unlike the [inline download](download.inline.md)
  ring, the staging is slot-based (three fixed windows, no ring cursor), so there is **no would-be-wrap
  tail waste and no ring-exceed ceiling**: a read larger than a window — or larger than the whole
  triple-buffered staging — simply packs across successive windows, each slot drained and reused, so bulk
  readback of any size streams through. A future streaming system could claim the partial-window tail.

## dx12 implementation

- [`dx12_download_async.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_async.hh)
  / [`.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_async.cc) — the copy actor,
  the three-window packing, the in-flight-window drain, and the two fences. The transfer queue is the
  system's **own** `D3D12_COMMAND_LIST_TYPE_COPY` `ID3D12CommandQueue` (`_copy_queue`, separate from the
  upload system's — see "Why upload and download own separate transfer queues"); the staging buffer is a
  persistently-mapped `D3D12_HEAP_TYPE_READBACK` committed buffer of `window_bytes * 3`; the read is
  `ID3D12GraphicsCommandList::CopyBufferRegion`. The **staging fence** is the system's own `_window_fence`;
  the **completion fence** is the system's own `_completion_fence` (download-only).
- The **copy queue + download completion fence** are created in the system's `initialize` and torn down
  (actor drained, copy queue idled) in the system's `shutdown`, called from
  [`dx12_context.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_context.cc) `shutdown`. The
  system owns one `ID3D12GraphicsCommandList` (reused across windows) plus one `ID3D12CommandAllocator` per
  window slot, cycled on the window fence — deliberately **not** the shared epoch-gated pool.
- The **forward wait vs a pending async upload** is `_copy_queue->Wait(_upload_async._completion_fence, ...)`,
  hoisted per window in `submit_window` on the max `_pending_async_upload_value` over its reads. The
  acyclicity guard extends to it: the actor closes the open window before staging a read whose pending-upload
  value is still unsatisfied once the window has finished a read. This replaces an earlier CPU block (which,
  on the old shared queue, could deadlock — see the fuzz op in `tests/transfer/transfer-fuzz-test.cc`).
- **Async texture readbacks assume the texture is already in the COMMON layout** — the copy queue runs no
  layout barriers, the same as the async upload. A freshly-created texture qualifies; one left in a
  shader/attachment layout by a direct-queue list must be transitioned back or read inline instead.
- The per-resource stamps live on `dx12_buffer`: `_pending_async_download_value` (reverse, a
  `dx12_download_fence_value` distinct from the epoch / submission / async-upload fences) and the shared
  `_last_used_submission_token` (forward). `track_buffer_access` in
  [`dx12_command_list.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc) folds
  the reverse value into `_required_download_wait` **only for write accesses** (`sg::is_unordered_write`).
  The reverse wait is `_queue->Wait(_download_async._completion_fence, ...)` at command-list submit; the
  forward wait is
  `_copy_queue->Wait(_submission_fence, ...)` before a window's reads in
  [`dx12_download_async.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_async.cc),
  which also enforces the window-level acyclicity rule (close the open window before staging a job whose
  forward token is still pending once the window has already finished a read).
- The readback recorder is
  [`dx12_resource_download.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_resource_download.hh)'s
  `dx12_buffer_download` (shared with inline download; made **resumable** so a read larger than a window
  chunks across calls), and the waiter is `dx12_async_download_waiter` — a `bytes_waiter` that simply blocks
  on its ready flag (no "submitted" gate, unlike the inline path — an async download is always handed to the
  actor).
- The public facade is [`context.download.hh`](../../src/shaped-graphics/context.download.hh)
  (`ctx.download`), which also carries the inline readback ring's `set_budget`.

## Backend note: vulkan needs a second-transfer-queue fallback

The separate-queue design assumes async upload and async download can each hold their **own** transfer
queue. dx12 grants this freely — `CreateCommandQueue` makes as many `COPY` queues as wanted (WARP included).
Vulkan does **not**: queues come from **queue families fixed at device creation**, and a dedicated
transfer family often exposes `queueCount == 1`, so two independent transfer queues are not guaranteed. A
vulkan backend must select capability-driven with a fallback — a second queue from a transfer-capable family
(graphics/compute queues implicitly support transfer) when available, else route one stream onto another
queue, or fall back to a single shared queue. **A single shared transfer queue reintroduces the FIFO
deadlock above**, so the shared-queue fallback must serialize upload/download differently (e.g. the old CPU
block, or one actor for both) rather than run two independent actors on it. This is unimplemented — the
vulkan backend is still a stub — but the constraint is load-bearing for its design.

## See also

- [async upload](upload.async.md) — the CPU→GPU mirror; async download reuses its per-buffer stamps with the
  forward/reverse roles swapped, and its window-level acyclicity rule.
- [inline download](download.inline.md) — the main-queue, record-in-list sibling; async's counterpart.
- [epochs](epochs.md) — the main-queue timeline; async download is deliberately decoupled from it.
- [threading](threading.md) — the `cc::threaded_actor` the copy runs on.
- [cheat-sheet](../../cheat-sheet.md) — the upload/download API at a glance.
