---
created: 2026-10-28
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_context.cc
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/transient.cc
---

# Metal places transient textures in a texture heap

**Metal refuses a `memory_heap_contents::textures` heap, so `ctx.transient` creates every metal texture dedicated.**
dx12 and vulkan place theirs in a per-epoch texture heap since 2026-10-28.
On dx12 that took a 14 ms CPU cost out of Sponza's hybrid frame: about six texture creations and frees per frame, each blocking in the driver.

Most of it exists already:
- `metal_memory_heap` is a placement heap, and `create_metal_texture` already places a texture with `newTexture(descriptor, offset)`.
- Missing is `query_texture_requirements`, which `heapTextureSizeAndAlign` answers, and accepting `textures` in `metal_context::try_create_memory_heap`.

It was left out only because metal cannot be built on the Windows machine that made the change.
What to check once it is in: a placed render target's first use must not read what an earlier epoch left in the memory.
The two transient texture tests in `tests/transient/transient-test.cc` cover both, and run on metal unchanged.
