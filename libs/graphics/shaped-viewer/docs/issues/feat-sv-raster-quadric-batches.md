---
created: 2026-10-17
parent: epic-sv-raster-path
refs:
  - file: libs/graphics/shaped-viewer/docs/raster.md
  - symbol: sv::generate_material_shader
  - symbol: sv::raster_routine
---

# Draw quadric batches on the raster path

A raster layer skips its quadric batches today, and warns once; a pending batch draws its placeholder box like a pending mesh.
The plan is a pixel stage that ray-casts `quadric.intersect` over each primitive's box and writes its own depth.
`generate_material_shader` emits no raster source for a quadric permutation until then.
