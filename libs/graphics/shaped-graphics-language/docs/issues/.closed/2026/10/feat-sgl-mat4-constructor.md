---
created: 2026-10-18
refs:
  - file: libs/graphics/shaped-graphics-language/tests/interpret/driver-bindings-test.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-20T13:13Z
closed: 2026-10-20
resolution: fixed
---

# A `mat4` cannot be constructed in SGL source

A matrix reaches a shader only through a binding, so a test of a matrix builtin binds its values from C++.
`determinant`'s values are such a test (`tests/interpret/driver-bindings-test.cc`); a constructor from four columns would let them move into the corpus.
`mat2` and `mat3` are left until a shader wants one.
