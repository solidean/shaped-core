---
created: 2026-11-03
tags: [sgl, autodiff]
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/flatten_diff.cc
closed: 2026-11-06
resolution: fixed
---

# Matrices have no +, - nor scale by a scalar

The prelude's `mat2`, `mat3` and `mat4` multiply, transpose and hand out columns, and nothing else.
A sum, a difference, a negation and a scale by a scalar are each missing, which a program notices as soon as it blends two matrices.

The autodiff prototype needs all four: a product rule over matrices sums two matrix terms, and an adjoint accumulates into a matrix.
Until the registry has them, `differentiator::matrix_operation` in flatten_diff.cc writes them column by column, marked `TEMPORARY(debt-sgl-matrix-arithmetic)`.
Each is one registry record per matrix type and a spelling per target; HLSL, WGSL and MSL all have them natively.
Closing this means deleting `matrix_operation` and its call.
