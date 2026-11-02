---
created: 2026-10-17
refs:
  - file: examples/vdoc/cube-editor/picking.hh
  - file: examples/vdoc/cube-editor/example-editor.cc
  - symbol: cube_editor::ray_aabb_entry
---

# cube-editor's picking keeps its own slab test, which tg now has

`picking.hh` hand-writes a ray / aabb slab test, because tg had no query module when it was written.
tg's `geometry/query/` now answers that pair: `ray.intersection_parameter_with(box)` gives the hit interval, and `ray.intersects(box)` the predicate.
Replace `ray_aabb_entry` with the tg query, and drop the `<limits>` include it needed.
