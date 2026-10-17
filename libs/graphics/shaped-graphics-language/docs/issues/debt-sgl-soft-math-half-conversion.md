---
created: 2026-10-17
blocked-by: [feat-cc-half-float]
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/impl/soft_math.hh
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/impl/soft_math.cc
  - symbol: sgl::builtins::impl::half_bits_of
  - symbol: sgl::builtins::impl::float_of_half_bits
---

# SGL hand-rolls its half conversions

`half_bits_of` and `float_of_half_bits` in `builtins/impl/soft_math.cc` round from `f32` and from `f64`, and keep subnormals, infinities and NaNs.
They stand in for a half type clean-core does not have yet; typed-geometry's `tg::f16` is out of reach, since SGL depends on clean-core alone.
Once clean-core has one, the two go and the interpreter converts through it.
