---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/scene/mesh_attribute.hh
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/scene/triangle_geometry.hh
  - symbol: sv::mesh_attribute::create
---

# `per_edge` attributes need an edge table on `triangle_geometry`

The enumerator exists and `mesh_attribute::create` rejects it.
What is missing is the numbering: the edges themselves (each naming its two vertices) plus each triangle's three edge indices.
That table also decides whether opposite half-edges share one entry, which is the real design question.
