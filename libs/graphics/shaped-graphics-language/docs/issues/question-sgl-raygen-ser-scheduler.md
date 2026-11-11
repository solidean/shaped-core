---
created: 2026-11-11
parent: epic-sgl-every-stage-everywhere
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/gpu-driven-work.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/raytracing-futures.md
---

# Does a raygen with SER beat compute for divergent work?

A raygen shader may reorder its threads by a coherence hint with no trace at all, which DXR 1.2 spells `MaybeReorderThread(hint, bits)`.
The hardware regroups threads by the hint, which is what a compute kernel with heavy divergence wants, and what a queue of mixed work-list records wants.
So a divergent kernel, or a work list's records, could run as a raygen that loops: pop a record, reorder by its pipeline or branch, run it.

The restrictions are real: no workgroup memory, no barriers, no subgroup layout to rely on after a reorder, and only dx12 and vulkan with SER.
SGL already writes `maybe_reorder` on both (CHK-433, EMIT-225), so a first measurement needs no new language.

To answer, measured on dx12 and vulkan, on NVIDIA first:

- A divergent compute kernel (a material switch, a wavefront path tracer's shading step) as compute, and as a reordering raygen.
- A small work list of mixed records as vulkan's device-generated commands, as the level-by-level polyfill, and as a reordering raygen.

The outcome decides whether this is a polyfill chosen by measurement, an explicit tool for divergent compute, or neither.
