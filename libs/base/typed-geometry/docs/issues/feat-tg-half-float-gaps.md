---
created: 2026-10-17
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/half_float.hh
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/impl/half_float_convert.hh
  - symbol: tg::f16
---

# `half_float`: what the first cut leaves out

- **Other small float formats.**
  bfloat16 and the 8-bit floats are the trigger to generalize `half_float` into a template over the exponent and fraction widths.
  Until a second format exists, the NaN and infinity rules such a template needs are guesses.
- **An `fma` for f16** has to compute in f64: through f32 it is not correctly rounded.
- **cl.exe on ARM64** has no half type, so it takes the portable conversion there; NEON intrinsics would give it the hardware path.
- **Span conversions using SIMD picked at run time** wait for a caller.
  The CPU feature query they needed now exists as `cc::get_cpu_features()`.
