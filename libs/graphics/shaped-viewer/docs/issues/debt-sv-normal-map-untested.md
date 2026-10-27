---
created: 2026-10-17
parent: epic-sv-openpbr-fidelity
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/openpbr_frame.sgl
  - symbol: openpbr.perturb_frame
---

# A normal map is still untested

`geometry_normal` is applied through `openpbr.perturb_frame` now, so it has somewhere to land, but no material in the tree binds it to a texture.
The same is true of `geometry_coat_normal`, which lands through the authored frame beside it.
