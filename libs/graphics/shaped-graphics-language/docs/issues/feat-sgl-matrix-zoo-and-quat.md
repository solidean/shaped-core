---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
---

# The matrix zoo, and `tg` types such as `quat`

A hit's transforms are rows of `float4` because SGL has `mat4`, and a `mat3` of function bodies alone.
`mat3x4` and a quaternion would let a hit hand them over typed.
Neither `mat4` nor `mat3` has a transpose, a subscript of a column, `==`, or a `mat3` of a `mat4`'s upper left, which a normal matrix wants (CHK-397).
`mat3` in GPU memory is `feat-sgl-arrays-and-mat3-in-gpu-memory`'s.
