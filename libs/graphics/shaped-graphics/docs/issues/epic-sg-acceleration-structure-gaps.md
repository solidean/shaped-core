---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/docs/concepts/acceleration-structures.md
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/raytracing/acceleration_structure.hh
---

# Acceleration structures: what is still open

See [concepts/acceleration-structures.md](../concepts/acceleration-structures.md).
The abstract types already carry the stats a refit needs — build and update scratch sizes, and the flags.

**Metal implements refit, compaction and placement natively, and sg exposes none of them.**
Recorded here so the eventual surface is designed against three APIs rather than two.

- `MTL4::ComputeCommandEncoder` carries `refitAccelerationStructure(source, descriptor, destination, scratch)`, with `VertexData` / `PerPrimitiveData` options.
- Beside it are `copyAndCompactAccelerationStructure` and `writeCompactedAccelerationStructureSize`.
- `MTL::Device::accelerationStructureSizes` already returns the refit scratch, which is the number `update_scratch_size_in_bytes()` holds a slot for.
- Placement is `heapAccelerationStructureSizeAndAlign` plus `MTL::Heap::newAccelerationStructure(size, offset)`.

All three APIs support refitting in place and into a separate structure, so a refit call would not be a Metal shape the others get bent into.
