# typed-geometry TODO

Running list of known follow-ups.
Add entries as we discover them; remove them as they land.

## scalar

- **Replace the `std::` math routing.** `scalar_traits<f32>` / `scalar_traits<f64>` dispatch `tg::sqrt` and the trig functions (`sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`atan2`) to `std::`, which honors `errno` — a historic mistake that also produces worse codegen (the compiler must keep the errno side effect). Replace these with direct hardware / builtin paths without the errno contract.
  Keep the `scalar_traits` seam so custom scalar types are unaffected.
- **Combined `sincos`.** `tg::sin_cos` currently calls `sin` and `cos` separately; with libm we can use the combined `sincos` entry point, which is cheaper.
  Add it as a `scalar_traits` operation and have `sin_cos` prefer it.

## linalg

- **`tg::rotor<D, T>`.** The transform module stores a rotation as an impl-local unit complex number in 2D and a `quat` in 3D.
  A public rotor — scalar plus bivector, generalizing both — would give the 2D rotation a name and a public composition operator, and let `transform/` stop dispatching on `D`.
- **Decomposition.** `homogeneous_transform` deliberately has no `make_from_mat` for the rotation and similarity classes: recovering them needs a polar or SVD decomposition, which belongs in `linalg/decomposition.hh`. Until it lands, build those classes from their factories.

## transform

- **Transforms between two different dimensions.** `homogeneous_transform` carries a source and a target dimension, and every signature is written in terms of the pair — but the type `static_assert`s that they are equal, so lifting and projecting are not implemented.
  What is missing: a storage that carries both dimensions (`transform_storage` still takes one), the rectangular cases of `composed` and `linear_mat`, and a decision on which capability classes even make sense off the diagonal — a rotation and a scaling are square by nature, `linear` / `affine` / `projective` are not.
- **Faster rigid/similarity paths for `plane`.** The plane registration goes through the cofactor matrix for every class.
  That is correct everywhere and exact for a rigid transform, but a rigid one could just rotate the normal.
  Add the fast path if it ever shows up in a profile.

## geometry

- **`obb`.** Missing, and it is what a rotated `aabb` becomes — until it exists, that combination is a deliberate compile error.
- **The affine image of an embedded `sphere`.** It is an `ellipsoid<D, DAmbient, T>`, but building one means turning the circle's normal into an orthonormal basis of its plane, which `linalg` has no routine for.
  What is missing is a `tg::any_orthogonal(vec)` / `tg::orthonormal_basis(vec)`; until it lands, that pair is a compile error while every non-embedded sphere maps fine.
- **`quadric`.** What a `sphere` or `ellipsoid` becomes under a projective map, so those pairs are unregistered rather than approximated.
- **A clipped / half-open segment.** What a `ray` becomes under a projective map, for the same reason.
- **`ball`.** The solid counterpart to `sphere`, reusing the same `{center, radius}` encoding, the way the planned `halfspace` will reuse `plane`'s.
