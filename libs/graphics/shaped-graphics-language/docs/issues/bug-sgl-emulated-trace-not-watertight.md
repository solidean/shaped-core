---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
  - file: libs/graphics/shaped-graphics-language/docs/raytracing-polyfill.md
  - test: emulated-trace*
---

# The emulated trace's triangle test is not watertight

Möller–Trumbore with a determinant cutoff misses or doubles a ray through an edge two triangles share ([raytracing-polyfill.md](../raytracing-polyfill.md#traversal)).
A watertight test is what hardware promises, and until the traversal has one, a test of it keeps its rays off shared edges.
