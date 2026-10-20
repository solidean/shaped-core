---
created: 2026-10-17
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/transform/homogeneous_transform.hh
  - symbol: tg::homogeneous_transform::make_from_mat
---

# Polar / SVD decomposition, and `make_from_mat` for rotations and similarities

`homogeneous_transform` deliberately has no `make_from_mat` for the rotation and similarity classes.
Recovering them needs a polar or SVD decomposition, which belongs in `linalg/decomposition.hh`.
Until it lands, build those classes from their factories.
