---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/docs/concepts/epochs.md
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/transient.cc
---

# A texture-capable transient heap

`ctx.transient`'s bump allocator is buffers-only.
So a transient texture falls back to a dedicated allocation the backend auto-expires at the next epoch, rather than being suballocated.
See [concepts/epochs.md](../concepts/epochs.md).
