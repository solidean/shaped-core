---
created: 2026-11-05
tags: [webgpu, raster]
parent: epic-sv-raster-bound
blocked-by: [feat-sv-raster-bound-skeleton, bug-sg-webgpu-indirect-indexed-draw-under-node]
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/raster-bound.md
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/shader_generator.cc
---

# `raster_bound`: textures through material bind groups, and large assemblies (C1)

- **Textures** reach a material through its own bind group; the generated texture load is the one seam that changes.
- **Attributes stored per corner or per triangle** expand to per vertex at load, for vertex buffers with positions in their own stream.
- **glTF over fetch** in the web harness.
- **The assembly benchmark**: 1k, 10k and 100k parts, 1% to 100% unique, 4 to 1024 materials.
  It draws them by direct calls, a render bundle, a bundle with GPU culling, merged per material, and instanced, timing CPU recording and GPU on each device.

Samples: a textured glTF, a CAD assembly built from typed-geometry parts.
