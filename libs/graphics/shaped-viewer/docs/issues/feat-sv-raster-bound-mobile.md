---
created: 2026-11-05
tags: [webgpu, raster, mobile]
parent: epic-sv-raster-bound
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/raster-bound.md
---

# `raster_bound` on phones: tiles, MSAA against TAA, and compile cost (C5)

- **Tiles**: store only what a later pass reads, discard depth and the MSAA samples, and measure whether the depth prepass pays on a tiler.
- **Antialiasing**: MSAA on mobile and TAA on desktop, each benchmarked on both so the choice has a cost attached.
- **Compile cost**: pipeline creation on a phone, with the generated WGSL shipped, against an OpenPBR uber-shader.
- **Textures**: ASTC or ETC2 where BC is absent.

Sample: every earlier sample, on a phone.
