---
created: 2026-11-05
tags: [webgpu, raster]
parent: epic-sv-raster-bound
blocked-by: [feat-sv-async-frame-api]
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/raster-bound.md
  - symbol: sv::render_mode
---

# `render_mode::raster_bound` draws untextured materials on WebGPU (C0)

The first checkpoint of [raster-bound.md](../plans/raster-bound.md#checkpoints):
- the viewer's frame loop runs on a never-block context;
- a web harness builds sv samples as single-file pages, as `sgl-cube` does, served over https on the LAN for phones;
- `raster_bound` draws with the frame, material and draw groups, materials as constants alone, lit directly as `raster.shade` lights;
- a dx12 path withholding what WebGPU lacks, so the suite tests the mode natively.

Also on the way: the pixel stage's `@primitive_id` needs WebGPU's optional `primitive-index`, and a de-indexed fallback where it is missing.
Sample: OpenPBR spheres, lit directly.
