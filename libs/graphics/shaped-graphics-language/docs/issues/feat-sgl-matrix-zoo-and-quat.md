---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
---

# The matrix zoo, and `tg` types such as `quat`

A hit hands its placement over as an `affine3` (CHK-408), and every matrix reads a column and a row by name (CHK-407).
What is left:

- **A quaternion**, mirroring `tg`'s, for rotations a hit or an animation carries.
- **`==` of matrices**, and **a `mat3` of a `mat4`'s upper left**, which a normal matrix wants.
- **`affine3` in GPU memory**: three `float4` rows, 48 bytes, the instance layout every API uses.
  The host needs a type of its own, since `tg::affine_transform3f` holds a column-major linear part and a translation instead.
- **A general 3x4 matrix product**, which the bgfx corpus's `41-tess` shaders need: `mul(xf, mtxFromRows(v0, v1, v2))` stacks three vertices as the rows of a matrix and blends them by a 3x3 one.
  That has no translation in it, so it is no `affine3`; the translations still write it column by column, marked `// gap:`.

`mat3` in GPU memory is `feat-sgl-arrays-and-mat3-in-gpu-memory`'s.
