# Concept: inline upload

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [epochs](epochs.md), [threading](threading.md), and the sibling [inline download](download.inline.md).

## What inline upload is

**Inline upload** stages latency-critical CPU→GPU buffer writes through a persistently-mapped
**UPLOAD-heap ring buffer** on the direct queue. `cmd.upload.bytes_to_buffer` memcpys the source
bytes into the ring and records a `CopyBufferRegion` **immediately** into the recording list — so the
destination buffer is usable by later commands in the *same* list. It is the "inline" path because the
copy is inlined into the caller's command stream, not deferred to a separate transfer submission.

The trade-off is capacity: the ring bounds how many bytes an epoch may upload inline before it must
wait for space. It is the right tool for per-frame, small-to-medium, must-be-visible-now writes
(constants, instance data, dynamic geometry), not for bulk asset streaming.

## Why a ring buffer, keyed to epochs

The mapped bytes an upload writes are read by the GPU when it executes the recorded copy — which
happens **later**, after the list is submitted and the queue reaches it. So the ring region cannot be
overwritten until the GPU is done reading it. That "is the GPU done?" question is exactly what
[epochs](epochs.md) answer cheaply, so the upload ring reclaims space at **epoch granularity**:

- The ring is a single **logical `u64` cursor** (`next_pos`) mapped onto the physical buffer via
  modulo. Windows never wrap — a would-be wrap wastes the tail and restarts at 0, keeping every window
  contiguous for a single `CopyBufferRegion`.
- At **advance**, `on_epoch_advance(closed)` snapshots the cursor as the closing epoch's boundary (an
  `epoch_checkpoint`).
- At **retire**, `on_epochs_completed(completed)` walks the checkpoints and advances the free
  watermark (`freed_pos`) past every epoch the GPU has finished. Those bytes are now reclaimable.

A reservation fits when `end - freed_pos <= capacity`. When it doesn't, the recording thread retires
the oldest in-flight epoch (`wait_for_next_inflight_epoch`) to advance the watermark and retries. If
**nothing** is in flight and it still doesn't fit, this one epoch's uploads exceed the ring — a hard
budget error (asserted), the documented v1 limitation.

## Where the pieces live

- [`dx12_upload_inline.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_upload_inline.hh)
  / [`.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_upload_inline.cc) — the ring,
  the reservation loop, and the epoch hooks. The system **creates and maps its own UPLOAD heap** in
  `initialize` (colocated with the logic that uses it), off the context's device.
- [`dx12_resource_upload.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_resource_upload.hh)
  — the per-resource copy recorder (`dx12_buffer_upload`), hiding buffer vs texture behind a job loop
  so the ring code is resource-agnostic (buffers stage in one job; chunked textures will use the loop).
- The epoch hooks are called from [`dx12_epoch.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_epoch.cc)
  `advance_epoch` / `process_completed_epochs`.

## Load-bearing invariants

Preserve these; the rest is tuning:

1. **The copy is recorded inline** into the caller's list — the destination is valid for later commands
   in the same list, with no extra submission.
2. **Space is reclaimed per epoch, gated on the epoch fence** — never freed while the GPU may still be
   reading the staged bytes.

## Current simplifications (deferred)

Not invariants — v1 shortcuts, each with a known better route:

- **Windows never wrap: a would-be wrap wastes the tail and restarts at 0.** Keeps every window
  contiguous for one `CopyBufferRegion`, at the cost of some slack near the seam. Splitting a staged
  copy across the seam into two `CopyBufferRegion`s (as the earlier prototype does) removes the waste —
  a straightforward improvement, deferred.
- **A single epoch's inline uploads must currently fit the ring** — over-budget asserts today. The
  intended fix is a fallback route that always works (e.g. a one-off committed staging buffer for the
  overflow), trading peak throughput for correctness rather than failing.

## Contrast with inline download

Upload and download are near-mirror ring buffers, but reclaim differs. Upload's staged bytes are
consumed by the **GPU**, so the epoch **fence** is the whole gate — retire frees the space. Download's
readback bytes are consumed by a **CPU actor** *after* the GPU writes them, so its space frees on a
per-epoch **actor-drain** signal, not at GPU retire. See [inline download](download.inline.md).

## See also

- [inline download](download.inline.md) — the GPU→CPU mirror and why its reclaim is actor-driven.
- [epochs](epochs.md) — the reclamation gate this rides on.
- [cheat-sheet](../../cheat-sheet.md) — the upload/download API at a glance.
