---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
  - symbol: procedural_of_mixed
---

# A generic struct's type parameter is not in scope in its methods

`mixed_hit.procedural()` returns through the helper `procedural_of_mixed[A]`, since the method cannot name `A`.
