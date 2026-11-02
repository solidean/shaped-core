---
created: 2026-10-17
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/transform/homogeneous_transform.hh
  - symbol: tg::homogeneous_transform
  - symbol: tg::transform_representation
---

# Transforms between two different dimensions

`homogeneous_transform` carries a source and a target dimension, and every signature is written in terms of the pair.
But the type `static_assert`s that they are equal, so lifting and projecting are not implemented.

What is missing: a representation that carries both dimensions (`transform_representation` still takes one), and the rectangular cases of `composed` and `linear_mat`.

Off the diagonal it also needs a decision on which capability classes even make sense.
A rotation and a scaling are square by nature, `linear` / `affine` / `projective` are not.
