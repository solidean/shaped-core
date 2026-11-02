---
created: 2026-10-30
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/builtins.sgl
  - file: libs/base/typed-geometry/src/typed-geometry/linalg/bivec.hh
---

# A bivec3 for normals, as typed-geometry has

SGL splits `float3` (component-wise) from `vec3` (a direction) and `pos3` (a point), mirroring `tg`.
`tg` has a fourth: a normal is a `bivec3`, which a transform carries by its cofactor rather than by the matrix itself.
SGL has no such type, so a normal is a `vec3`, and `mat3 * normal` is accepted though it is wrong under a non-uniform scale.

- **What it would buy**: the inverse-transpose mistake becomes a type error, and a normal crosses to the host as `tg::bivec3f`.
- **What it would cost**: `cross` of two `vec3` is a `bivec3`, so `cross`, `reflect`, `normalize` and the tangent frame all need deciding again.
  Every port then carries one more conversion.
- **Undecided**: whether this pays off in shader code, where most normals are read from a vertex and only ever normalized and dotted.

It came up while deciding how `float3` and `vec3` meet in the bgfx corpus (`question-sgl-bgfx-corpus-friction`), where `mat3` gains `vec3` columns.
