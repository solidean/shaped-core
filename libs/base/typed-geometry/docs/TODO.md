# typed-geometry TODO

Running list of known follow-ups. Add entries as we discover them; remove them as they land.

## scalar

- **Replace the `std::` math routing.** `scalar_traits<f32>` / `scalar_traits<f64>` dispatch
  `tg::sqrt` and the trig functions (`sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`atan2`) to `std::`,
  which honors `errno` — a historic mistake that also produces worse codegen (the compiler must keep
  the errno side effect). Replace these with direct hardware / builtin paths without the errno
  contract. Keep the `scalar_traits` seam so custom scalar types are unaffected.
- **Combined `sincos`.** `tg::sin_cos` currently calls `sin` and `cos` separately; with libm we can
  use the combined `sincos` entry point, which is cheaper. Add it as a `scalar_traits` operation and
  have `sin_cos` prefer it.
