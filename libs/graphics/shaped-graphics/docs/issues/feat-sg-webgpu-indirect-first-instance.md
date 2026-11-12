---
created: 2026-11-05
tags: [webgpu, raster]
parent: epic-sv-raster-bound
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/capabilities.hh
  - issue: bug-sg-webgpu-indirect-indexed-draw-under-node
---

# Request WebGPU's `indirect-first-instance`

An indirect draw with a non-zero first instance needs WebGPU's optional `indirect-first-instance`, which sg does not request.
Without it an instanced part cannot name its row of the instance table through the draw, which sv's bound raster mode relies on.
Possibly related: [bug-sg-webgpu-indirect-indexed-draw-under-node](bug-sg-webgpu-indirect-indexed-draw-under-node.md) suspects the first-instance field.
