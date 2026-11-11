---
created: 2026-11-11
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_shade.sgl
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/builtin_material_types.cc
  - symbol: micro_visibility
  - symbol: sv_parallax
---

# Self-shadowing for openpbr_relief, from a horizon map

`openpbr_relief` has micro-occlusion as a visibility per direction and parallax occlusion mapping at every traced hit, but its relief casts no shadow on itself.
On a shingled roof that shadow is what reads: each butt's shadow on the course below; the parallax of a 10 mm lap is barely visible at a normal camera distance.

Why not march the height toward the light: light sampling in `shade_sampled` is shared code, not the per-type generated code that `sv_parallax` is, so it cannot read a material's height texture.

The plan:

- **Two optional rgba attributes**, the horizon elevation in 8 directions of tangent space, baked by the producer from the height.
- **A surface field** beside `micro_occlusion`, the horizon interpolated toward the direction asked.
- **Applied like the micro-occlusion**: on light samples, environment samples and the continuation alike, so multiple importance sampling weighs one visibility.
- **A reference test**: a step height field's shadow against the same step as geometry, plus mutation checks, like the parallax test.

## Known limitation to fix with it

Emission textures are read at the parallax offset in the shading context, while next-event estimation reads emission at level 0 and no offset.
No relief material is emissive yet, so nothing shows it.
