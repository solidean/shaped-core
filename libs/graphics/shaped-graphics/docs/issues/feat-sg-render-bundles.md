---
created: 2026-11-05
tags: [webgpu, raster]
parent: epic-sv-raster-bound
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/raster-bound.md
---

# Render bundles: record binds and draws once, replay them every frame

A WebGPU draw is one call from wasm, and multi-draw indirect is still behind a flag, so a CAD assembly of 10k parts costs 10k calls a frame.
A render bundle records the binds and indirect draws once; its indirect arguments are read when it runs, so GPU culling still decides each frame.
WebGPU has `GPURenderBundle`; dx12 has bundles, vulkan secondary command buffers, metal indirect command buffers, or a CPU recording where none fits.
sv's bound raster mode wants it ([raster-bound.md](../../../shaped-viewer/docs/plans/raster-bound.md#drawing-many-parts)), measured against direct calls in its assembly benchmark.
