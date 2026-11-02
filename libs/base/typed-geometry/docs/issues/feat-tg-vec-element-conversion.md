---
created: 2026-10-17
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/linalg
---

# Converting a linalg type's element type

A `vec<3, f16>` becomes a `vec<3, f32>` element by element today, and so does every other pair of scalar types.
The spelling — a converting constructor on each linalg type, or one generic `tg::convert<To>(from)` — waits for a caller.
