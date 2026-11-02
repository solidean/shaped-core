---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/scene/mesh_flags.hh
  - symbol: sv::mesh_flag
---

# Nothing consumes `sv::mesh_flag` yet

`mesh` and `resident_mesh` carry `mesh_flags` — `visible`, `casts_shadow`, `receives_shadow` — and nothing in the renderer reads them.
So a caller clearing one gets no effect and no diagnostic.
The set is provisional and will grow as the renderer learns to honor it: pickability, backface culling, wireframe overlay.
