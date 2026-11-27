---
created: 2026-10-17
parent: epic-sg-transfer-gaps
tags: [dx12]
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_upload_async.cc
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_download_async.cc
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_transfer.cc
  - file: libs/graphics/shaped-graphics/tests/transfer/stream-test.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-27T14:15Z
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

## Resolution

Every async and streaming readback on dx12 now waits on the greater of the async and the streaming upload stamp, which share one timeline.
The reverse edge was missing too, and wider than the issue said: no dx12 upload waited on any readback, async or streaming.
An upload now carries a download wait, counted as pending under the same acyclicity guard as its direct-queue token.
Vulkan had both edges for async transfers, and its uploads now also wait on a streaming readback, textures included.
`stream-test.cc` pins both edges with a gated source; without the change, dx12 read zeroes ahead of the stream, and in the reverse case read the later upload's bytes.
