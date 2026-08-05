# typed-geometry TODO

Running list of known follow-ups.
Add entries as we discover them, and remove them as they land.

## scalar

- **Replace the `std::` math routing.**
  `scalar_traits<f32>` / `scalar_traits<f64>` dispatch `tg::sqrt` and the trig functions (`sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`atan2`) to `std::`, which honors `errno`.
  The compiler must then preserve that side effect, which costs codegen for a contract nobody wants.
  Replace them with direct hardware or builtin paths, keeping the `scalar_traits` seam so custom scalar types are unaffected.
- **Combined `sincos`.**
  `tg::sin_cos` calls `sin` and `cos` separately, where libm's combined `sincos` entry point is cheaper.
  Add it as a `scalar_traits` operation and have `sin_cos` prefer it.
