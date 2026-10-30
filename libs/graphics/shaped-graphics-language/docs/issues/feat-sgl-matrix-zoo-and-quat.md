---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
---

# The matrix zoo, and `tg` types such as `quat`

A hit's transforms are rows of `float4` because SGL has `mat4`, and a `mat3` of function bodies alone.
`mat3x4` and a quaternion would let a hit hand them over typed.
Neither `mat4` nor `mat3` has a subscript of a column, `==`, or a `mat3` of a `mat4`'s upper left, which a normal matrix wants (CHK-397).
`mat3` in GPU memory is `feat-sgl-arrays-and-mat3-in-gpu-memory`'s.

The bgfx corpus in shader-rosetta needs two more, each written today through a stand-in marked `// gap:`:

- **A 3x4 matrix**, bgfx's `mat4x3` with `mtxFromRows` and `mtxGetRow`, in `41-tess/cs_terrain_lod` and `41-tess/vs_terrain_render`.
- **A column or an element of a matrix**, bgfx's `mtxGetColumn` and `mtxGetElement`, in `41-tess/cs_terrain_lod`.
