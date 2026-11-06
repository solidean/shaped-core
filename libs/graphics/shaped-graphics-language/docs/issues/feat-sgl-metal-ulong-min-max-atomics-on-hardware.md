---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_sync.cc
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_context.cc
claimed-by: Claude (for Julius Nehring-Wirxel)
claimed-at: 2026-11-06T09:25Z
---

# Run metal's atomic ulong min and max on Apple hardware

SGL writes a buffer's `atomic[ulong]` `min` and `max` as MSL 3.1's `atomic_min_explicit` and `atomic_max_explicit` (EMIT-218), checked by emit tests alone.
sg's metal backend answers `shader_int64_min_max_atomics` from `MTLGPUFamilyApple8`, which is Apple's feature set tables as read, not a device run.
What is left is a GPU test on an Apple GPU that has them, and one that lacks them refusing the pipeline.
