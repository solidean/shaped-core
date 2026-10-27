---
created: 2026-10-17
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/scalar.hh
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/angle.hh
  - symbol: tg::sin_cos
---

# `tg::sin_cos` makes two calls where libm has one

`tg::sin_cos` calls `sin` and `cos` separately, where libm's combined `sincos` entry point is cheaper.
Add it as a `scalar_traits` operation and have `sin_cos` prefer it.
