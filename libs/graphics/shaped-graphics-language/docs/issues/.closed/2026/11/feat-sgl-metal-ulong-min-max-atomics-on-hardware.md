---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_sync.cc
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_context.cc
closed: 2026-11-06
resolution: fixed
---

# Run metal's atomic ulong min and max on Apple hardware

SGL writes a buffer's `atomic[ulong]` `min` and `max` as MSL 3.1's `atomic_min_explicit` and `atomic_max_explicit` (EMIT-218), checked by emit tests alone.
sg's metal backend answers `shader_int64_min_max_atomics` from `MTLGPUFamilyApple8`, which is Apple's feature set tables as read, not a device run.
What is left is a GPU test on an Apple GPU that has them, and one that lacks them refusing the pipeline.

## Resolution

On 2026-11-06 `sg - an SGL compute shader's ulong atomic min and max compare all 64 bits of a buffer's element` ran on an M4, the first run on hardware.
It found that every standing min or max was followed by `(void)();`, which the Metal compiler rejects: the emit tests checked the call and not what followed it.
The statement writer now writes nothing for a value that left no text, and the emit test checks for it.
The refusal on a device without the feature (an M1, Apple7) stays unrun here; it is sg's generic feature gate, which other features already exercise.
