---
created: 2026-11-05
tags: [webgpu, raster]
parent: epic-sv-raster-bound
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/raster-bound.md
---

# `raster_bound`: screen-space AO, specular occlusion and reflections (C4)

Ambient occlusion by GTAO or a visibility bitmask, specular occlusion from the same horizons, and hierarchical-Z reflections falling back to probes or the environment.
On a tiler these need depth and normals written out: a deliberate small write, at half resolution.
Each technique is checked for patents before code.
Sample: a showroom with a glossy floor.
