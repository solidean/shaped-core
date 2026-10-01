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
- **`fixed_int`: what the first cut leaves out.**
  Mixed signed / unsigned heterogeneous operands, and generated loop-free widening shifts.
  Generated bit counts and float conversions, both short loops today.
  The bounded quotient's remainder at the divisor's width, where the divisor's bound allows it.

- **`half_float`: what the first cut leaves out.**
  bfloat16 and the 8-bit floats are the trigger to generalize `half_float` into a template over the exponent and fraction widths.
  Until a second format exists, the NaN and infinity rules such a template needs are guesses.
  An `fma` for f16 has to compute in f64: through f32 it is not correctly rounded.
  cl.exe on ARM64 has no half type, so it takes the portable conversion there; NEON intrinsics would give it the hardware path.
  Span conversions using SIMD picked at run time wait for a caller; they need a CPU feature query clean-core does not have.

## linalg

- **Converting a vector's element type.**
  A `vec<3, f16>` becomes a `vec<3, f32>` element by element today, and so does every other pair of scalar types.
  The spelling — a converting constructor on each linalg type, or one generic `tg::convert<To>(from)` — waits for a caller.

- **`tg::rotor<D, T>`.**
  The transform module stores a rotation as an impl-local unit complex number in 2D and a `quat` in 3D.
  A public rotor — scalar plus bivector, generalizing both — would give the 2D rotation a name and a public composition operator, and let `transform/` stop dispatching on `D`.
- **Decomposition.**
  `homogeneous_transform` deliberately has no `make_from_mat` for the rotation and similarity classes: recovering them needs a polar or SVD decomposition, which belongs in `linalg/decomposition.hh`.
  Until it lands, build those classes from their factories.

## transform

- **Transforms between two different dimensions.**
  `homogeneous_transform` carries a source and a target dimension, and every signature is written in terms of the pair.
  But the type `static_assert`s that they are equal, so lifting and projecting are not implemented.
  What is missing: a representation that carries both dimensions (`transform_representation` still takes one), and the rectangular cases of `composed` and `linear_mat`.
  Off the diagonal it also needs a decision on which capability classes even make sense.
  A rotation and a scaling are square by nature, `linear` / `affine` / `projective` are not.
- **Faster rigid/similarity paths for `plane`.**
  The plane registration goes through the cofactor matrix for every class.
  That is correct everywhere and exact for a rigid transform, but a rigid one could just rotate the normal.
  Add the fast path if it ever shows up in a profile.

## geometry

- **`quadric`.**
  What a `sphere` or `ellipsoid` becomes under a projective map, so those pairs are unregistered rather than approximated.
- **A clipped / half-open segment.**
  What a `ray` becomes under a projective map, for the same reason.
