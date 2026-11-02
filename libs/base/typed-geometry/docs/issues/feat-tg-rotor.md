---
created: 2026-10-17
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/transform/impl/rotation_representation.hh
---

# `tg::rotor<D, T>`

The transform module stores a rotation as an impl-local unit complex number in 2D and a `quat` in 3D.
A public rotor — scalar plus bivector, generalizing both — would give the 2D rotation a name and a public composition operator.
It would also let `transform/` stop dispatching on `D`.
