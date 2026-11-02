---
created: 2026-10-17
parent: epic-sg-transfer-gaps
tags: [dx12]
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_upload_async.cc
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_async.cc
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_transfer.cc
  - file: libs/graphics/shaped-graphics/tests/transfer/stream-test.cc
---

# An async transfer does not order against an in-flight stream of the same resource

Command lists do: their access tracking reads the stream stamps alongside the async ones, waits, and warns once per stream.
The async tier does not — a download job reads `_pending_async_upload_value` and no stream value, and the upload side mirrors that.
So `ctx.stream.bytes_to_buffer` followed by `ctx.download.bytes_from_buffer` on one resource is unordered, and the readback can beat the stream.

Found while writing [stream-test.cc](../../tests/transfer/stream-test.cc)'s stream-wait test, whose first draft used the async tier as the consumer and read zeroes on dx12.
The fix mirrors what the command lists already do, in dx12's async enqueue paths.

**Metal already does it**: its streaming timeline is per resource, so an async transfer waits on the same value a command list would.
That is also what makes `promote_to_async` a pure statement of intent there.
The reverse edge there — a *stream* ordering behind an in-flight async transfer — was missing until the tier-1 sweep read zeroes out of a texture an async upload was still filling.
`order_stream_copy` now waits on the transfer timeline as well.
