---
created: 2026-10-27
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_upload_async.cc
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/transfer/stream.hh
---

# A region-scoped stream into a buffer still makes every list touching the buffer wait

`stream_scope::region` promises that only the streamed byte range is claimed, and that the rest of the buffer may be used concurrently ([stream.hh](../../src/shaped-graphics/transfer/stream.hh)).
On dx12 it is not honored for a buffer: `stream_source_buffer` stamps the buffer's `_pending_stream_copy_value` whatever the scope.
`fold_transfer_waits` folds that value for every touched buffer, so any list touching the buffer waits for the whole transfer, and warns that it does.

Seen in sv, when the mesh manager streamed each mesh into a region of its central vertex pool (2026-10-27).
The single-threaded build failed 13 viewer tests on the undeclared warning; the threaded build passed only because the streams landed before the next frame.
sv now streams into per-mesh staging and copies it into the pool, which costs a copy and transient memory per mesh.

Measured on dx12 alone.
Whether vulkan and metal honor the scope is not checked; both carry the same warning text.
The fix is likely a stamp that deferred deletion reads but command-list tracking does not, for a region-scoped buffer stream.
