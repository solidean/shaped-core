---
created: 2026-11-11
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/raster_bound_indirect.sgl
---

# raster_bound's GI and reflection histories test the plane but not the instance

The half-size shadow and occlusion history continues a texel only where it remembers this pixel's instance and its point lies on this pixel's plane.
The instance test is what fixed the halo around load-asset's pillar feet.
TAA's jitter moves a texel at a pillar's foot between the floor and the pillar, which lie on one plane where they touch, so a plane test alone averaged both.

The traced indirect light's history and the traced reflections' history (`reprojected` in `raster_bound_indirect.sgl`) have the plane test only.
They have no channel to remember an instance in: the radiance target is `rgba16_float` with the frame count in w, and the depth target is `r32_float`.
No artifact has been traced to them yet; the same crease leak is what to look for, on GI at a quarter of the size, where it would spread wider.
