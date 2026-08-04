# Concept: async upload

## What async upload is

**Async upload** streams CPU→GPU buffer writes on a **dedicated transfer (copy) queue**, off the frame path.
`ctx.upload.bytes_to_buffer` hands the source bytes to a background [`cc::threaded_actor`](threading.md) and returns immediately.
The bytes are held alive by a [`cc::pinned_data`](../../../../base/clean-core/src/clean-core/container/pinned_data.hh).
The actor memcpys them into a persistently-mapped staging buffer and records a copy on the transfer queue.
It is the context-level mirror of the inline [`cmd.upload`](upload.inline.md).
`cmd.upload` records into a command list and is visible to later commands in it; `ctx.upload` is decoupled from any list or epoch.

It is the right tool for **bulk asset streaming** — large, not-needed-this-instant writes that must not stall the recording thread or burn main-queue time.
For small, must-be-visible-now per-frame writes, use [inline upload](upload.inline.md).

**Fire-and-forget.** The call returns `void`.
The source bytes are only read during the memcpy into staging, so the caller may free the original immediately — the pin keeps them alive until the copy consumes them.
Inline upload copies synchronously instead.
Empty data is a no-op.

## Why sync is automatic in both directions (the load-bearing decision)

The staged copy runs **later**, asynchronously, on a queue the caller never sees.
For the CPU timeline `submit → async upload → submit` to just work — no future to poll, no manual barrier — the system needs sync in **both directions**.
Each direction is a **cross-queue GPU wait**, never a CPU stall, and both ride on two per-resource stamps reserved synchronously at the record call.

**Forward — a later reader waits on the copy.** A command list that reads the buffer after the upload must not execute until the copy has run, or it reads stale bytes.

- The upload reserves a **completion value** on a monotonic counter and **stamps it onto the buffer** (atomic max) before the actor runs.
  Any reader recorded afterwards therefore sees a value to wait on.
- Every op that reads a buffer while recording folds that buffer's completion value into the list's required wait.
  That covers uploads, downloads, buffer copies, and a buffer bound into a binding group.
- At **submit**, the main queue is told to **wait on the transfer queue's completion fence** for that value before executing the list.
  One wait per list covers every buffer it reads.

**Reverse — the copy waits on an earlier user.**
A command list submitted *before* the upload that uses the buffer must finish before the copy overwrites it, or the copy races an in-flight reader or writer.

- Each command list, at submit, **stamps every buffer it used with its submission token** (atomic max).
- The upload reads that token when recorded and carries it.
  When the actor submits the window holding the copy, it first makes the **transfer queue wait on the main queue's submission fence** for that token.
  One wait per window covers every buffer copied in it.

Both waits point strictly **backward in the CPU submission order**, so *per operation* the dependency graph is acyclic — no deadlock — and the timeline is easy to reason about.
Multiple async uploads to the **same** buffer compose correctly.
They are processed in order, their copies stay in order on the transfer queue, and each waits on a token at least as high as the previous, so the last upload wins.

**One scheduling rule keeps that acyclic at the window level.**
The reverse wait is issued once per *window*, on the max token over its copies, hoisted ahead of the window's execute.
So a window must never *both* signal a completion `V` *and* carry a reverse wait that transitively depends on `V`.
The hoisted wait would then sit ahead of the very copy whose signal it needs, closing a cycle.
The actor enforces this by **closing the open window before staging a job** whose reverse token is still pending on the direct queue, once the window has already finished an upload.
Each window's reverse wait then points only at prior, already-submitted windows or at already-complete tokens.

Over-waiting on a higher (monotonic) value is always safe, and neither stamp is ever reset — a stale value only ever yields a cheap already-satisfied wait.
sg has **no per-resource state / access-tracking layer yet**, so this pair of per-buffer stamps is a deliberately minimal stand-in that the in-progress access-tracking layer should subsume.

The **happens-before rule** the caller relies on: an upload is ordered against command lists whose *submit* (reverse) or *record* (forward) happens before or after the upload call on the same thread.
A buffer concurrently used by a list recording in parallel with the upload is a hazard the caller must avoid — v1 is persistent-buffer, single-writer.

## Why staging is triple-buffered (pipelining)

The staging buffer is split into a small number of fixed-size **windows** — **three**.
Only the actor thread fills them, so there is no concurrent-recording hazard and no per-epoch reclaim bookkeeping.
What remains is a per-window **staging fence** the transfer queue signals as each window's copy completes.
A window's size is a backend config knob, and can also be changed at runtime — see the resize note in the dx12 section below.

Three windows is the load-bearing count, because it lets **CPU memcpy and GPU copy overlap**:

- The actor fills the current window and **submits it as soon as it fills** — or as soon as the inbox drains, for low latency — then rolls to the next window.
- It reuses a window's memory only **three submissions later**, so on coming back to a window that window's copy is almost always already done and the reuse wait is free.
  At any instant one window is being copied by the GPU, one was just submitted, and one is being filled by the CPU.

With fewer windows the actor would stall on the window it just handed the GPU, a sync bubble; more only adds staging memory.
An upload **larger than one window** simply **packs across successive windows**.
Each window copies its slice, and the window holding the upload's last byte is the one whose completion satisfies the reader wait.

Two fences on the transfer queue, both signaled per window:
- the **staging fence** — every window, gating window reuse and thus reclaim;
- the **completion fence** — signaled up to the *highest finished upload value* in that window, skipped when a window carries only a mid-upload chunk, and what the main queue waits on.

Finished upload jobs, and their pins, are destroyed on the actor thread.
That is off the submission path, so releasing a large pin never stalls a latency-sensitive thread.

## Load-bearing invariants

Preserve these; the rest is tuning:

1. **Sync is automatic in both directions.**
   Forward: a later command list that reads the buffer waits on the copy — completion value stamped before enqueue, main queue waits at submit.
   Reverse: the copy waits on the last command list that used the buffer — submission token stamped at submit, transfer queue waits before the copy.
   No future, no manual barrier; waits point backward in submission order, so the graph is acyclic.
2. **The source pin outlives the memcpy into staging**, and the job is then destroyed on the actor thread — the caller may free its bytes as soon as the call returns.
3. **Triple-buffered windows keep CPU and GPU overlapped** — a window is reused only after enough others have been submitted that its copy is, almost always, already done.

## Current simplifications (deferred)

Not invariants — v1 shortcuts:

- **Persistent buffers only**, and **single-writer**: an async upload to a buffer concurrently used by an in-flight list is the caller's hazard to avoid.
- **In-order copies (head-of-line blocking).**
  Copies run strictly in submission order on the transfer queue, so a reverse wait on a slow command list stalls *all* later async copies behind it, not just the ones on that buffer.
  Pulling blocked jobs out of order and filling around them is the deferred optimization, and it has to keep same-buffer uploads composing.
- **Coarser than per-buffer state**: the stamps are single monotonic values per buffer, a down-payment on the per-resource state-tracking layer landing separately, which should replace them.
- **No CPU-observable completion** — no `upload_token`, no future.
  Completion is expressed purely as the automatic GPU wait; a cheap poll on the completion fence could be exposed later if a "safe to reference now" signal is wanted.
- **A future streaming system** can claim the unused tail of a partially-filled window instead of submitting it early, which v1 does not need.

## dx12 implementation

- [`dx12_upload_async.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_upload_async.hh)
  / [`.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_upload_async.cc)
  — the copy actor, the three-window packing, and the two fences.
  The transfer queue is the system's **own** `D3D12_COMMAND_LIST_TYPE_COPY` `ID3D12CommandQueue` (`_copy_queue`).
  It is separate from the download system's, so their windows never FIFO-block each other — see [async download](download.async.md).
  The staging buffer is a persistently-mapped `D3D12_HEAP_TYPE_UPLOAD` committed buffer of `window_bytes * 3`, and the copy is `ID3D12GraphicsCommandList::CopyBufferRegion`.
  The **staging fence** is the system's own `_window_fence`; the **completion fence** is its own `_completion_fence`, upload-only.
- The **copy queue + completion fence** are created in the system's `initialize`, alongside the staging buffer.
  They are torn down — actor drained, copy queue idled — in the system's `shutdown`.
  That is called from [`dx12_context.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_context.cc) `shutdown`.
  The system owns one `ID3D12GraphicsCommandList` (reused across windows) plus one `ID3D12CommandAllocator` per window slot, cycled on the window fence.
  Deliberately **not** the shared epoch-gated `dx12_command_allocator_pool`, which is for resources that observe epoch semantics.
- The per-resource stamps live on `dx12_buffer`: `_pending_async_upload_value` (forward) and `_last_used_submission_token` (reverse).
  The forward one is a `dx12_copy_fence_value`, distinct from the epoch and submission fences.
  `track_buffer_access` in [`dx12_command_list.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc) reads it into `_required_copy_wait`.
  It also records the buffer, so submit stamps the reverse one with the list's token.
  The forward wait is `_queue->Wait(_upload_async._completion_fence, ...)` at command-list submit.
  The reverse wait is `_copy_queue->Wait(_submission_fence, ...)`, issued before a window's copies.
  `stage_job` in [`dx12_upload_async.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_upload_async.cc) enforces the window-level acyclicity rule.
  Once the open window has finished an upload, it closes that window before staging a job whose reverse token is still pending.
  So a window never both signals a completion and waits on a token that depends on it.
- **Async texture copies assume the texture is already in the COMMON layout** — the copy queue can't run layout barriers.
  A freshly-created texture qualifies.
  One left in a shader/attachment layout by a direct-queue list must be transitioned back first, or uploaded inline (`cmd.upload.bytes_to_texture` drives the barrier).
- **Resource lifetime spans the copy queue.**
  The copy queue is decoupled from epochs, so a buffer whose last reference is dropped while an async upload to it is still in flight must not be freed when its epoch retires.
  Deferred deletion carries a second gate: each expiring resource is tagged with the buffer's `_pending_async_upload_value`.
  `process_completed_epochs` holds it back on a re-checked `copy_deferred` list until the copy fence reaches that value.
  See [`dx12_epoch.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_epoch.hh)
  / [`dx12_epoch.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_epoch.cc).
- **A dropped upload still signals its value.**
  The job holds only a `std::weak_ptr<dx12_buffer const>`, locked at stage time.
  If every handle was dropped, or the storage expired, before the actor got there, `stage_job` skips the copy — a large upload to a released buffer never stages or blocks.
  It still folds the job's completion value into the window, so `submit_window` signals the copy fence up to it.
  That is mandatory: the fence is monotonic, and both the lifetime gate above and any forward reader stamped with the value wait on it, so a hole would hang them.
  A window whose jobs were all dropped still submits an empty list and signals.
  Only the `CopyBufferRegion` and the reverse `wait_token` fold are skipped, since no copy means no reverse hazard.
- **The staging window resizes at runtime.**
  `ctx.upload.set_async_window_size(bytes)` records a new window size, which the copy actor adopts at the top of its next process cycle, between windows.
  It submits any open window, fully drains the copy queue so no in-flight window still reads the old buffer, then rebuilds the triple-buffered staging buffer at `bytes * 3`.
  The per-slot allocators and the reused command list survive; only staging memory changes.
  Applied before the next upload is staged, so in-flight uploads are unaffected.
- The public facade is [`upload.hh`](../../src/shaped-graphics/context/upload.hh), reached as `ctx.upload` — see [context](context.md).

## See also

- [context](context.md) — the scope this hangs off, and how it relates to the other five.
- [inline upload](upload.inline.md) — the main-queue, record-in-list sibling; async's counterpart.
- [async download](download.async.md) — the GPU→CPU mirror; it reuses these per-buffer stamps with the forward/reverse roles swapped, plus a CPU memcpy completion step.
- [inline download](download.inline.md) — why its ring needs per-epoch counters that async does not.
- [epochs](epochs.md) — the main-queue timeline; async upload is deliberately decoupled from it.
- [threading](threading.md) — the `cc::threaded_actor` the copy runs on, and what a build without threads must pump.
- [cheat-sheet](../../cheat-sheet.md) — the upload/download API at a glance.
