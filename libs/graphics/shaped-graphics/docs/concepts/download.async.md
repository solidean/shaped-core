# Concept: async download

## What async download is

**Async download** streams GPU→CPU buffer readback on a **dedicated transfer (copy) queue**, off the frame path.
`ctx.download.bytes_from_buffer` hands the read to a background [`cc::threaded_actor`](threading.md) and returns a [`bytes_future`](../../src/shaped-graphics/bytes_future.hh) immediately.
The actor records a copy from the source into a persistently-mapped **readback** staging buffer on the transfer queue.
It then memcpys the staged bytes into the caller's destination and settles the future.
It is the context-level mirror of the inline [`cmd.download`](download.inline.md).
`cmd.download` records into a command list and is delivered once that list runs; `ctx.download` is decoupled from any list or epoch.

It is the right tool for **bulk readback** — asset baking, screenshots, GPU→CPU result streaming — that must not stall the recording thread or burn main-queue time.
For a small must-be-back-now readback tied to a specific list's output, use [inline download](download.inline.md).

**Fire-and-return-a-future.** The call returns a `bytes_future`; the copy runs later, on a queue the caller never sees.
Block on it with `ctx.wait_for(future)`, or poll `future.is_ready()` / `future.try_get_bytes()`.
The returned `pinned_data` keeps the bytes alive on its own, so it stays valid past the future's lifetime.
A zero-size read yields an already-ready, empty future.

## Why sync is automatic in both directions (the load-bearing decision)

The read runs **later**, asynchronously, on a queue the caller never sees.
For the CPU timeline `submit → async download → submit` to just work with no manual barrier, the system needs sync in **both directions**.
Each direction is a **cross-queue GPU wait** rather than a CPU stall, and it reuses the same per-resource stamps [async upload](upload.async.md) established, with the roles mirrored.

**Forward — the read waits on the last writer.** A command list that wrote the buffer before the download must finish before the read runs, or the read observes stale bytes.

- Each command list, at submit, **stamps every buffer it used with its submission token** (atomic max) — the same stamp async upload's reverse wait uses.
- The async download reads that token when recorded and carries it.
  When the actor submits the window holding the read, it first makes the **transfer queue wait on the main queue's submission fence** for that token.
  One wait per window covers every buffer read in it.

**Forward — the read also waits on a pending async upload.**
An async *upload* to the same buffer runs on a **separate** transfer queue (see below), so it is not covered by the submission-fence wait.
The download reads the buffer's pending-async-upload value when recorded and carries it, and the window holding the read **waits on that buffer's upload timeline** for the value before executing.
That is a clean cross-queue GPU wait, so `submit-upload → async-download` of the same buffer just works with no CPU stall.

**Reverse — a later writer waits on the read.** A command list that **writes** the buffer after the download must not overwrite the bytes while the transfer queue is still reading them.

- The download reserves a **completion value** on a monotonic counter and **stamps it onto the buffer** (atomic max) before the actor runs.
  Any writer recorded afterwards therefore sees a value to wait on.
- Every op that **writes** a buffer while recording folds that buffer's download value into the list's required wait — **only writes**, since two reads never conflict.
- At **submit**, the main queue is told to **wait on that buffer's download timeline** for the value before executing the list.
  The transfer queue signals it once the window holding the read has completed, so the write lands strictly after the read.

Both waits point strictly **backward in the CPU submission order**, so *per operation* the dependency graph is acyclic — no deadlock.
Multiple async downloads of the **same** buffer are independent reads; two reads never conflict, so they need no ordering against each other.

Reads are picked **out of order across sources**, keyed by an ordering family, exactly as [async upload](upload.async.md) picks copies.
So a read blocked behind a slow command list is filled around rather than stalling every later readback behind it.

**One scheduling rule keeps that acyclic at the window level.**
Each forward wait — the submission token *and* the upload-completion value — is issued once per *window*, on the max over its reads, hoisted ahead of the window's execute.
So a window must never *both* signal a completion `V` that a later writer waits on *and* carry a forward wait that transitively depends on `V`.
The hoisted wait would then sit ahead of the very read whose signal it needs, closing a cycle.
The actor enforces this exactly as async upload does, with the same two order-independent rules.
A read with either wait still pending may not join a window that has already finished a read; and once such a read *is* in the open window, no other read may join it.
A read's own completion is safe beside its own waits, since both were captured before its value was reserved.

**Why upload and download own separate transfer queues.** A `Wait` on a GPU queue blocks *all* work behind it in that queue's FIFO.
If upload and download shared one queue, an upload window waiting on a direct-queue token, and a download window queued behind it that would release that token, could **deadlock**.
The per-window acyclicity rule above only reasons about one actor's own windows, not about the other actor's work sharing the queue.
Giving each system its own queue removes that cross-actor coupling: the only ordering between them is the explicit upload-completion wait above, which points strictly backward.
On backends where a second transfer queue is not guaranteed this needs a fallback — see the vulkan note below.

## Why a download completes with a CPU memcpy (and how windows drain)

Unlike async upload, where staging a copy is the last the actor does with a job, a download only finishes once the **CPU memcpy** out of the readback staging buffer has run.
That post-GPU step is what a download's `bytes_future` becomes ready on.

The staging buffer is triple-buffered into fixed **windows** ([three](upload.async.md), same as upload), so GPU read and CPU memcpy overlap.
But because the memcpy must run *after* the window's GPU read, each submitted window is kept **in flight** until it is **drained**.
To drain a window the actor waits on that window's staging fence, memcpys its chunks into their destinations, and settles their futures.
It drains a window:

- **before reusing its slot** — a slot is reused three submissions later, and reuse must wait not only for the GPU read but for the CPU memcpy too.
  Otherwise it would overwrite bytes the memcpy still needs.
- **for every remaining in-flight window when the inbox empties**, before the actor sleeps — a sleeping actor must leave nothing undrained, or a `bytes_future` would never become ready.
  No epoch advance forces it; only the actor's memcpy does.

During bulk streaming this pipelines to depth three: one window read by the GPU, one just submitted, one filled by the CPU.
When the inbox drains it flushes.
A read **larger than one window** packs across successive windows.
The window holding the read's last byte carries the completion value and the future's completion node.
Because windows drain in order, the earlier chunks are already copied by the time that node is settled.

Two kinds of fence on the transfer queue, both signaled per window:
- the **staging fence** — one per system, signaled every window, gating window reuse and thus drain ordering;
- a **completion timeline per source resource** — signaled up to the *highest value that resource finished* in the window, and what a later writer's reverse wait blocks on.
  A window carrying only mid-read chunks finishes nothing and signals nothing.

Completion is per resource rather than per system, for the reason [async upload](upload.async.md) gives.
Reads select out of order, so a shared counter would report an older read finished the moment a newer one did.

## Why the source is held strong (and the future's pin weak)

The job holds a **strong** handle to the source buffer for its whole lifetime, so the source storage stays alive across the transfer-queue read.
This is the one place async download is *simpler* than async upload.
Upload holds a `weak_ptr` so a dropped-target upload can be skipped, and it needs a deferred-deletion **copy gate** to keep storage alive past the strong lock.
Download's strong hold makes that gate unnecessary.

The **destination** is held **weak** — a `weak_ptr` on the future's pin — so **dropping the future cancels the copy at the next opportunity:**

- If the pin has expired by the time the actor reaches the job (**stage time**), the read is skipped entirely — no `CopyBufferRegion`, no forward wait.
  A large read to an abandoned future therefore never touches the GPU.
  But the job's **completion value is still folded** into a window so its timeline reaches it, because a later writer stamped with that value must not hang.
  That is exactly async upload's dropped-target rule: a cancelled job still signals its value, and only the copy is skipped.
- If the pin expires **after** the read was recorded (**drain time**), the memcpy is skipped and the staged bytes are simply not copied out.

## Load-bearing invariants

Preserve these; the rest is tuning:

1. **Sync is automatic in both directions.**
   Forward: the read waits on the last command list that wrote the buffer — submission token stamped at submit, transfer queue waits before the read.
   Reverse: a later command list that *writes* the buffer waits on the read — completion value stamped before enqueue, main queue waits at submit.
   Only writes fold in the reverse wait; waits point backward in submission order, so the graph is acyclic.
2. **A download completes with a CPU memcpy on the actor**, deferred until the window's GPU read finishes.
   It never stalls the recording thread and never needs an epoch advance, and every window is drained before its slot is reused and before the actor sleeps.
3. **The source is held strong across the read**, so no copy gate is needed; the destination is held weak, so dropping the future cancels the copy.
   A cancelled read still signals its reverse-sync completion value.

## Current simplifications (deferred)

Not invariants — v1 shortcuts:

- **Persistent buffers only**, and **single-writer**: an async download of a buffer concurrently written by an in-flight list is the caller's hazard to avoid.
- **Coarser than per-buffer state**: the stamps are single monotonic values per buffer, a down-payment on the per-resource state-tracking layer landing separately, which should replace them.
- **Submits a partially-filled window early** when the inbox drains, rather than claiming its unused tail for the next job — a low-latency choice, not a size limit.
  Unlike the [inline download](download.inline.md) ring, the staging is slot-based (three fixed windows, no ring cursor), so there is **no would-be-wrap tail waste and no ring-exceed ceiling**.
  A read larger than a window, or larger than the whole triple-buffered staging, simply packs across successive windows, each slot drained and reused, so bulk readback of any size streams through.
  A future streaming system could claim the partial-window tail.

## dx12 implementation

- [`dx12_download_async.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_async.hh)
  / [`.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_async.cc)
  — the copy actor, the three-window packing, the in-flight-window drain, and the two fences.
  The transfer queue is the system's **own** `D3D12_COMMAND_LIST_TYPE_COPY` `ID3D12CommandQueue` (`_copy_queue`).
  It is separate from the upload system's — see "Why upload and download own separate transfer queues".
  The staging buffer is a persistently-mapped `D3D12_HEAP_TYPE_READBACK` committed buffer of `window_bytes * 3`, and the read is `ID3D12GraphicsCommandList::CopyBufferRegion`.
  The **staging fence** is the system's own `_window_fence`; completion rides the per-resource timelines above, so the system owns no completion fence of its own.
- The **copy queue** is created in the system's `initialize`.
  They are torn down — actor drained, copy queue idled — in the system's `shutdown`.
  That is called from [`dx12_context.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_context.cc) `shutdown`.
  The system owns one `ID3D12GraphicsCommandList` (reused across windows) plus one `ID3D12CommandAllocator` per window slot, cycled on the window fence.
  Deliberately **not** the shared epoch-gated pool.
- The **forward wait vs a pending async upload** is one `_copy_queue->Wait(group->fence, value)` per source's upload timeline.
  It is hoisted per window in `submit_window`, on the max `_pending_async_upload_value` over that window's reads.
  The acyclicity guard extends to it: once the open window has finished a read, the actor closes it before staging a read whose pending-upload value is still unsatisfied.
  The fuzz op in `tests/transfer/transfer-fuzz-test.cc` covers the interleaving this protects.
- **Async texture readbacks assume the texture is already in the COMMON layout** — the copy queue runs no layout barriers, the same as the async upload.
  A freshly-created texture qualifies.
  One left in a shader/attachment layout by a direct-queue list must be transitioned back, or read inline instead.
- The per-resource stamps live on `dx12_buffer`: `_pending_async_download_value` (reverse) and the shared `_last_used_submission_token` (forward).
  The reverse one is a `dx12_download_fence_value`, distinct from the epoch / submission / async-upload fences.
  `track_buffer_access` in [`dx12_command_list.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc) folds the reverse value into `_required_download_wait`.
  It does so **only for write accesses** (`sg::is_unordered_write`), since two reads never conflict.
  The reverse wait is one `_queue->Wait(group->fence, value)` per download timeline, at command-list submit.
  The forward wait is `_copy_queue->Wait(_submission_fence, ...)`, issued before a window's reads.
  That path in [`dx12_download_async.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_async.cc) also enforces the window-level acyclicity rule.
- The readback recorder is `dx12_buffer_download` in [`dx12_resource_download.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_resource_download.hh).
  It is shared with inline download, and made **resumable** so a read larger than a window chunks across calls.
  Completion is a `cc::make_async_manual<cc::unit>` node the actor pushes after the memcpy, or pushes `make_cancelled()` on when the destination was dropped.
  It carries no `bytes_wait_gate`, unlike the inline path, because an async download is always handed to the actor and never waits on its caller.
- The public facade is [`download.hh`](../../src/shaped-graphics/context/download.hh), reached as `ctx.download` — see [context](context.md).
  It also carries the inline readback ring's `set_budget`.

## Backend note: vulkan needs a second-transfer-queue fallback

The separate-queue design assumes async upload and async download can each hold their **own** transfer queue.
dx12 grants this freely — `CreateCommandQueue` makes as many `COPY` queues as wanted, WARP included.
Vulkan does **not**: queues come from **queue families fixed at device creation**.
A dedicated transfer family often exposes `queueCount == 1`, so two independent transfer queues are not guaranteed.
A vulkan backend must therefore pick its queues capability-driven, with a fallback.
First choice is a second queue from a transfer-capable family, since graphics and compute queues implicitly support transfer.
Failing that, route one stream onto another queue, or fall back to a single shared queue.
**A single shared transfer queue reintroduces the FIFO deadlock above.**
That fallback must therefore serialize upload against download — one actor for both, say — rather than run two independent actors on it.
The vulkan backend is still a stub, but the constraint is load-bearing for its design.

## vulkan implementation

As on the [upload](upload.async.md#vulkan-implementation) side: the transfer queue emits **no image barrier at all**.
The texture is put in the layout the copy needs by the *direct* queue, before the readback is enqueued — see
[barriers](barriers.md#the-transfer-queue-never-changes-a-layout--the-direct-queue-settles-it-first) — and the
semaphore that orders this submit after that one also makes its writes visible here.
A transfer that claims no layout has none for the validation layer to disagree with, and that layer reads submit-call
order rather than GPU order.
Textures carry the same per-resource stamps buffers do, in both directions.

## See also

- [context](context.md) — the scope this hangs off, and how it relates to the other five.
- [async upload](upload.async.md) — the CPU→GPU mirror; async download reuses its per-buffer stamps with the forward/reverse roles swapped, and its window-level acyclicity rule.
- [inline download](download.inline.md) — the main-queue, record-in-list sibling; async's counterpart.
- [epochs](epochs.md) — the main-queue timeline; async download is deliberately decoupled from it.
- [threading](threading.md) — the `cc::threaded_actor` the copy runs on, and what a build without threads must pump.
- [cheat-sheet](../../cheat-sheet.md) — the upload/download API at a glance.
