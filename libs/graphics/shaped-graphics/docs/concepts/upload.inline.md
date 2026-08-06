# Concept: inline upload

## What inline upload is

**Inline upload** stages latency-critical CPU→GPU writes through a persistently-mapped **staging ring** in host-visible upload memory, on the **main (graphics) queue**.
`cmd.upload.bytes_to_buffer` memcpys the source bytes into the ring and records a buffer copy **immediately** into the recording list.
So the destination buffer is usable by later commands in that *same* list.
It is the "inline" path because the copy is inlined into the caller's command stream rather than deferred to a separate transfer submission.

The trade-off is capacity: the ring bounds how many bytes an epoch may upload inline before it must wait for space.
It is the right tool for per-frame, small-to-medium, must-be-visible-now writes — constants, instance data, dynamic geometry.
For bulk asset streaming see [async upload](upload.async.md).

## Why a ring buffer, keyed to epochs

The mapped bytes an upload writes are read by the GPU when it executes the recorded copy, which happens **later** — after the list is submitted and the queue reaches it.
So the ring region cannot be overwritten until the GPU is done reading it.
"Is the GPU done?" is exactly what [epochs](epochs.md) answer cheaply, so the upload ring reclaims space at **epoch granularity**:

- The ring is a single **logical cursor** over an unbounded byte count, mapped onto the physical buffer via modulo.
  A copy reserves its whole span at once, then walks it in windows capped at the ring end, so one that would straddle the wrap is **split at the seam**.
  Each recorded copy is therefore contiguous.
  A buffer copy wastes no tail; a texture copy abandons a tail too small for one aligned row and restarts at the seam.
- At **epoch advance**, the cursor is snapshotted as the closing epoch's boundary.
- At **epoch retire**, a free watermark advances past every epoch the GPU has finished, and those bytes become reclaimable.

A reservation fits when the requested window lies within `capacity` bytes of the free watermark.
When it does not, the recording thread retires the oldest in-flight epoch to advance the watermark, then retries.
If **nothing** is in flight and it still does not fit, this one epoch's uploads exceed the ring — a hard budget error, asserted.

## Load-bearing invariants

Preserve these; the rest is tuning:

1. **The copy is recorded inline** into the caller's list — the destination is valid for later commands
   in the same list, with no extra submission.
2. **Space is reclaimed per epoch, gated on the epoch fence** — never freed while the GPU may still be
   reading the staged bytes.

## Runtime resize

`ctx.upload.set_inline_budget(bytes)` records a pending ring capacity, applied at the next `advance_epoch`.
The apply drains every in-flight epoch, so no GPU work still reads the ring, then reallocates at the new size and restarts the logical cursor at 0.
Because reclaim is fence-gated (invariant 2), draining the epoch fence is enough — there is no actor to wait out, unlike the [inline download](download.inline.md) ring.
It runs only after a `set_budget`, so the stall is acceptable.

## Current simplifications (deferred)

Not invariants — v1 shortcuts, each with a known better route:

- **A single epoch's inline uploads must fit the ring** — over-budget asserts today.
  The intended fix is a fallback route that always works, such as a one-off dedicated staging buffer for the overflow, trading peak throughput for correctness rather than failing.

## Contrast with inline download

Upload and download are near-mirror ring buffers, but reclaim differs.
Upload's staged bytes are consumed by the **GPU**, so the epoch **fence** is the whole gate and retire frees the space.
Download's readback bytes are consumed by a **CPU actor** *after* the GPU writes them, so its space frees on a per-epoch **actor-drain** signal rather than at GPU retire.
See [inline download](download.inline.md).

## dx12 implementation

- [`dx12_upload_inline.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_upload_inline.hh)
  / [`.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_upload_inline.cc)
  — the ring (`next_pos` cursor, `freed_pos` watermark, `epoch_checkpoint` FIFO), the reservation loop, and the epoch hooks.
  The system **creates and maps its own `D3D12_HEAP_TYPE_UPLOAD` buffer** in `initialize`, off the context's device.
  The "copy command" is `ID3D12GraphicsCommandList::CopyBufferRegion`, on the single DIRECT queue.
- [`dx12_resource_upload.hh`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_resource_upload.hh)
  — the per-resource copy recorder, `dx12_buffer_upload` / `dx12_texture_upload`.
  It hides buffer vs texture behind a resumable job loop, so the ring stays a plain byte allocator.
  A buffer or texture larger than the free ring splits across the seam, the texture split at row/slice granularity.
- The epoch hooks `on_epoch_advance` / `on_epochs_completed` are called from `advance_epoch` / `process_completed_epochs` in
  [`dx12_epoch.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_epoch.cc).

## See also

- [async upload](upload.async.md) — the copy-queue sibling for bulk streaming (`ctx.upload`), off the frame path.
- [inline download](download.inline.md) — the GPU→CPU mirror and why its reclaim is actor-driven.
- [epochs](epochs.md) — the reclamation gate this rides on.
- [cheat-sheet](../../cheat-sheet.md) — the upload/download API at a glance.
