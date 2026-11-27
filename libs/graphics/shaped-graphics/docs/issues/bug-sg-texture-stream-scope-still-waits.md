---
created: 2026-11-27
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/types.hh
  - test: sg stream - a list touching the rest of a buffer a region streams into does not wait for it
---

# A subresource- or region-scoped stream into a texture still makes every list touching the texture wait

`stream_scope::subresource` promises that filling mip 5 leaves mips 0-4 free for sampling, and `region` the same within a subresource.
On dx12 and vulkan a texture stream stamps `_pending_stream_copy_value` whatever its scope, so a list sampling any mip waits for the whole stream, and warns.
Read from the code on 2026-11-27, not measured; the buffer half was bug-sg-region-stream-buffer-wait, fixed by a lifetime-only stamp.

A texture is not a buffer here, which is why that fix was not carried over.
The stamp also drives `has_pending_transfer`, which tracks a texture with a transfer in flight in `general`, and the async layout fixup orders against it.
A narrower stamp needs both of those to keep seeing the transfer while command-list waits stop seeing it.
