# typed-geometry TODO

Running list of known follow-ups. Add entries as we discover them; remove them as they land.

## scalar

- **Replace `std::sqrt`.** `scalar_traits<f32>` / `scalar_traits<f64>` currently dispatch
  `tg::sqrt` to `std::sqrt`, which honors `errno` — a historic mistake that also produces worse
  codegen (the compiler must keep the errno side effect). Replace it with a direct hardware
  square-root path (e.g. the relevant intrinsic / a builtin without the errno contract). Keep the
  `scalar_traits` seam so custom scalar types are unaffected.

## linalg

- **`comp` arithmetic.** `comp` is the raw component-wise building block and should eventually
  carry the full element-wise arithmetic surface (`+ - * /`, min/max, …). Currently it only
  stores and indexes.
- **`bivec` + `cross`/`dual`/`undual`.** `cross(vec3, vec3)` should return `bivec3`; needs the
  `bivec` type first.
- **`mat` / `quat`.** Column-major matrices with C++23 multi-argument `operator[]`.
