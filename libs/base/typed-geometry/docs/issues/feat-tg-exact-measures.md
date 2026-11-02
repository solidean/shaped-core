---
created: 2026-10-17
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/geometry/primitives/triangle.hh
  - symbol: tg::triangle::area
---

# Measures that stay exact on an exact scalar

`triangle::area()` is refused for an exact scalar, since halving an odd doubled area truncates.
A "twice the area" member stays in `T` there, and a tetrahedron's volume times six is its integer-exact sibling.
Spelling and placement wait for a caller.
