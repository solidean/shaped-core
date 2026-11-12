---
created: 2026-11-10
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/raster_bound.sgl
  - file: libs/graphics/shaped-viewer/shaders/sgl/raster_bound_shadows.sgl
---

# sv raster_bound: dark smoke walks along walls when the camera moves slightly

Reported in Firefox on the web-viewer with the modern hall: dark, smoke-like blotches travelled along walls, always toward the bottom-right.

**Cause, fixed 2026-11-10 and confirmed by the user:** the half-size grid was mapped half a full-size pixel off.
Half-size texel `h` holds full-size pixel `2h`, centered at `2h + 0.5`, so a full-size position `x` is half-size `(x - 0.5) / 2`.
`ao_accumulated` reprojected through `x / 2 - 0.5` instead, so it read a still history a quarter texel up and left every frame.
The history slid toward the bottom-right, without end once the still average was uncapped.
The shading's upsamples carried the same offset, statically.

Tried first without effect: a history test that also accepts the reciprocal depth interpolated across the four texels, for surfaces seen edge-on.
It stayed, since it is right on its own terms.
