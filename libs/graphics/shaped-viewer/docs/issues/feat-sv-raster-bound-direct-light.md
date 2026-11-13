---
created: 2026-11-05
tags: [webgpu, raster]
parent: epic-sv-raster-bound
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/raster-bound.md
---

# `raster_bound`: shadow maps, LTC rect lights and a prefiltered environment (C3)

Cascaded shadow maps for the sun, a cube per point light and a map per spot.
Rect lights have theirs already, up to 16 in one atlas ([raster-bound.md](../plans/raster-bound.md)).
Rect lights by linearly transformed cosines in place of the 4×4 grid.
A prefiltered environment with a split-sum lookup table in place of the blurred mirror term, and multiple-scattering energy compensation.
Each technique is checked for patents before code.
Sample: a showroom.
