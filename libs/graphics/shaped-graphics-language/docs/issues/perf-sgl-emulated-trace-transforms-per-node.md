---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
---

# The emulated trace transforms its ray into the instance at every node it pops

Each pop of a BLAS node reloads four pool entries, applies `apply_rows` to the origin and the direction again, and recomputes `inverse` with three divisions.
Those change only where the walk crosses into another instance.
`apply_rows(..., 0.0)` also writes `x.w * 0.0` terms that a compiler without fast-math cannot fold away.
The six `sg_acceleration_pool[instance_unit + 0..2]` loads stand twice, once in the `if owner >= 0` block and once where the hit is built.
L1 cannot merge them, since the first stand in a sibling scope.
A fix keeps the instance's ray in locals or on the stack, and takes a direction-only transform.
