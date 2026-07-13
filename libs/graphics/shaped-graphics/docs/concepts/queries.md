# Concept: GPU queries

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [inline download](download.inline.md), [epochs](epochs.md), and [threading](threading.md).

## What GPU queries are

A **GPU query** records a value the GPU produces while executing a command list, read back on the host
afterward. Today the only kind is a **timestamp**: `cmd.query.record_gpu_timestamp()` records a
point-in-time GPU tick at that point in the list and returns a
[`gpu_timestamp`](../../src/shaped-graphics/gpu_timestamp.hh). Two timestamps around some work give that
work's GPU duration — only *differences* between ticks are meaningful, never the absolute value.

The result surfaces as a small, copyable value type (the query analogue of a `bytes_future`):

- `is_valid()` — backed by a real recorded query. False for a default-constructed query and for a
  backend without timestamp support (`cmd.query.is_supported()` reports that up front).
- `is_ready()` — the tick has been read back to the host. Poll it; the normal per-frame usage is to
  read a timestamp a frame or two after recording, not to block on it.
- `try_get_ticks()` / `try_get_seconds()` — the raw GPU tick and the tick converted to seconds (via
  `1 / timestamp_frequency`), once ready.
- `ctx.wait_for_ticks(timestamp)` / `ctx.wait_for_seconds(timestamp)` — the blocking path (mirrors
  `wait_for` on a download); returns the raw tick / seconds once delivered. Waitable only after the
  recording list is submitted.

## How the dx12 backend implements it (the load-bearing decisions)

**Leased heaps, bump-allocated slots.** A [`dx12_query_system`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_query.hh)
owns a free-list **pool** of small (4096-slot) `ID3D12QueryHeap`s. A recording list **leases** a heap on
demand and bump-allocates one slot per `record_gpu_timestamp`, issuing `EndQuery(TIMESTAMP, slot)`. A
list that records more than a heap's worth simply leases another — there is no global cap. The heaps are
small on purpose: it mirrors the tighter query limits of other APIs and bounds each readback chunk.

**One batched readback per heap, at submit.** Resolving a query heap is GPU work, so it must be recorded
before the list closes. `dx12_command_list::finalize_queries_before_close` runs from `submit` (under the
submission lock, before `Close`): it allocates one transient buffer, `ResolveQueryData`s every leased
heap into a slice of it, and starts **one inline readback per heap** — reusing the ordinary
[inline-download](download.inline.md) path (`_download_inline.download_buffer`). Those readbacks ride the
list's existing `_pending_downloads`, so the submit path already stamps and enqueues them; nothing about
the download machinery is duplicated. The heaps then return to the pool immediately.

**Why a shared per-heap future.** At *record* time the readback does not exist yet, but the handle must
already reference where its tick will land. Each lease therefore carries a **shared, initially-invalid
`data_future<u64>`**; every timestamp of that heap aliases it (as `const`) and remembers its own slot
index. At submit, `finalize_queries_before_close` assigns each heap's future in place with the real
readback, and all its handles see it at once. This is why a **command list is single-threaded** and
results are read **after submit**: the in-place assignment is a single-writer event, and reading before
it (before submit) simply reports not-ready.

**Transient resolve buffer lifetime.** The resolve buffer is a current-epoch transient, so it stays
alive until the epoch retires — which cannot happen before the direct-queue resolve + readback copies of
that epoch complete. Creating it inside the submission lock is safe: transient creation touches only its
own heap/registry locks, never the submission lock.

**Drop cancels cleanly.** Dropping an unsubmitted list returns its leased heaps to the pool unresolved.
The handles it already handed out keep their (still-invalid) future forever, so they stay
`is_valid() && !is_ready()` and `wait_for` fails instead of hanging — exactly like a cancelled download.

## Extending to other query types

The `dx12_query_heap_type` enum, the per-type free list, and the lease/resolve/readback flow are shaped
to grow: occlusion or pipeline-statistics queries add a heap type and their own result view, and reuse
the same pooling and submit-time resolve machinery. Timestamps are the only kind implemented today.
