---
created: 2026-10-27
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_upload_async.cc
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/transfer/stream.hh
closed: 2026-11-27
resolution: fixed
---

# A region-scoped stream into a buffer still makes every list touching the buffer wait

`stream_scope::region` promises that only the streamed byte range is claimed ([stream.hh](../../../../../src/shaped-graphics/transfer/stream.hh)).
The rest of the buffer may be used concurrently.
On dx12 it is not honored for a buffer: `stream_source_buffer` stamps the buffer's `_pending_stream_copy_value` whatever the scope.
`fold_transfer_waits` folds that value for every touched buffer, so any list touching the buffer waits for the whole transfer, and warns that it does.

Seen in sv, when the mesh manager streamed each mesh into a region of its central vertex pool (2026-10-27).
The single-threaded build failed 13 viewer tests on the undeclared warning; the threaded build passed only because the streams landed before the next frame.
sv now streams into per-mesh staging and copies it into the pool, which costs a copy and transient memory per mesh.

Measured on dx12 alone.
Whether vulkan and metal honor the scope is not checked; both carry the same warning text.
The fix is likely a stamp that deferred deletion reads but command-list tracking does not, for a region-scoped buffer stream.

## Resolution

A region-scoped stream into a buffer stamps a lifetime slot of its own on dx12 and vulkan, which deferred deletion reads and nothing else does.
A readback with that scope stamps nothing on dx12, whose job holds the source, and the lifetime slot on vulkan.
webgpu no longer brings a region stream forward for a list touching the buffer.
`sg stream - a list touching the rest of a buffer a region streams into does not wait for it` fails without the change on dx12 and vulkan, on the stall warning.
Not covered: metal, which this machine cannot run, and textures, whose narrower scopes still make every list wait; bug-sg-texture-stream-scope-still-waits is that.
