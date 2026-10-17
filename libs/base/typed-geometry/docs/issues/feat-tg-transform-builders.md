---
created: 2026-10-17
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/linalg/mat.hh
  - symbol: babel::gltf::node
---

# Transform builders: quaternion → matrix, translation, scaling, TRS composition

Wanted: `tg::mat<3, 3, T>::make_from_quat(tg::quat<T>)`, `tg::mat<4, 4, T>::make_translation(tg::vec<3, T>)`, `make_scaling(tg::vec<3, T>)`.
And a `make_from_trs(translation, rotation, scale)` that composes the three in the conventional order, fixed once instead of per caller.

glTF is the direct case.
A node's local transform is either a 4×4 `matrix` or a translation / rotation / scale triple, mutually exclusive per the spec, and files in the wild use both.
So a consumer walking the node hierarchy has to turn the TRS form into a matrix before it can multiply anything.

Today `linalg/mat.hh` offers only `make_from_cols`, `make_rotation_x/y/z`, `make_rotation_axis_angle` (3×3 only), `zero` and `identity`.
So `babel::gltf::node` stores whichever form the file used (`has_matrix` says which) and the reader composes nothing.
That is a defensible native-structure decision on its own.
The gap is the missing `local_transform(node)` helper, which today would mean hand-rolling quaternion → matrix math inside babel.
