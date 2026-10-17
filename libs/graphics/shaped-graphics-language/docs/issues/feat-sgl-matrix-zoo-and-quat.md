---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
---

# The matrix zoo, and `tg` types such as `quat`

A hit's transforms are rows of `float4` because SGL has `mat4` alone.
`mat3x4`, `mat3` and a quaternion would let a hit hand them over typed.
`mat3` in GPU memory is `feat-sgl-arrays-and-mat3-in-gpu-memory`'s.
