---
created: 2026-10-17
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/traits.hh
  - symbol: tg::scalar_traits
---

# f32 / f64 math routes through std:: and pays for errno

`scalar_traits<f32>` / `scalar_traits<f64>` dispatch `tg::sqrt` and the trig functions (`sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`atan2`) to `std::`, which honors `errno`.
The compiler must then preserve that side effect, which costs codegen for a contract nobody wants.

Replace them with direct hardware or builtin paths, keeping the `scalar_traits` seam so custom scalar types are unaffected.
