---
created: 2026-11-11
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_shade.sgl
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/builtin_material_types.cc
  - symbol: micro_visibility
  - symbol: sv_parallax
closed: 2026-11-12
resolution: fixed
---

# Self-shadowing for openpbr_relief, from a horizon map

`openpbr_relief` has micro-occlusion as a visibility per direction and parallax occlusion mapping at every traced hit, but its relief casts no shadow on itself.
On a shingled roof that shadow is what reads: each butt's shadow on the course below; the parallax of a 10 mm lap is barely visible at a normal camera distance.

Why not march the height toward the light: light sampling in `shade_sampled` is shared code, not the per-type generated code that `sv_parallax` is, so it cannot read a material's height texture.

The plan:

- **Two optional rgba attributes**, the horizon elevation in 8 directions of tangent space, baked by the producer.
  `feat-sv-render-channels` gives a producer that bakes from a model the horizons traced against true geometry, not marched over a height.
- **A surface field** beside `micro_occlusion`, the horizon interpolated toward the direction asked.
- **Applied like the micro-occlusion**: on light samples, environment samples and the continuation alike, so multiple importance sampling weighs one visibility.
- **A reference test**: a step height field's shadow against the same step as geometry, plus mutation checks, like the parallax test.

## Done, 2026-11-12

- `openpbr_relief` takes `horizon_lo` and `horizon_hi`, the sines of the horizon in 8 directions of its tangent frame.
- The tracer interpolates the horizon at a direction's azimuth and closes what lies below it, a sine of 0.05 soft.
  It applies on light samples, environment samples and the continuation alike; where a horizon is bound it stands in for the micro-occlusion.
- Tested in pathtraced-view-test: a horizon of 45 degrees toward +t shadows a light from there at 30 degrees, and neither one at 60 nor one from -t.
- The horizon map itself is the producer's, from its height; machina-works packs one per mip level.
- The emission limitation this issue carried moved to `bug-sv-relief-emission-parallax-offset`.
