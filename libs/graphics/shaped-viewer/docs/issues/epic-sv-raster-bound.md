---
created: 2026-11-05
tags: [webgpu, raster]
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/raster-bound.md
---

# The bound raster mode: sv on WebGPU, without bindless and without ray-tracing pipelines

[raster-bound.md](../plans/raster-bound.md) is the plan: a second raster mode binding per material, forward shaded, with probe GI traced through the ray-query polyfill.
Its checkpoints are this epic's children, S0 first, each landing samples that run in a browser.
