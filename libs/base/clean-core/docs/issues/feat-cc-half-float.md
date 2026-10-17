---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/impl/soft_math.hh
  - symbol: tg::f16
---

# A half type: IEEE binary16 with correct rounding

**Wanted:** a 16-bit float in clean-core, converting from `f32` and from `f64` with one rounding to nearest, ties to even, and widening exactly.
typed-geometry's `tg::f16` is that type, so the wish is for it, or its conversion core, one library lower.

**Why:** SGL depends on clean-core alone, and must, since an editor links it to parse.
Its interpreter holds `half` values (CHK-381, EVAL-96), and every one of them is rounded from the 32-bit result its family computes.
A literal rounds from its exact `f64` value, since a detour through `f32` would round twice.

**Today:** SGL hand-rolls the conversions, `debt-sgl-soft-math-half-conversion`.
