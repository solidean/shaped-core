---
created: 2026-10-17
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/linalg/mat.hh
  - symbol: babel::gltf::node
  - issue: feat-tg-transform-builders
---

# An affine transform type, and `mat4 * pos3` / `mat4 * vec3`

Wanted: the transform type typed-geometry's roadmap already anticipates, plus the affine products that tell a point (translated) from a direction (not translated).

It is the other half of [feat-tg-transform-builders](feat-tg-transform-builders.md).
Once a glTF node's local transform exists, composing parent × child down the hierarchy and applying the result to positions and normals is the whole of placing a mesh in a scene.
`mat4f * vec4f` alone pushes every caller into homogeneous-coordinate bookkeeping the type system should be doing.

Today `tg::mat` multiplies `mat<C, R> * vec<C>` only, so there is no affine application at all.
babel does not attempt it: the node hierarchy comes back as parent / child indices, left un-flattened.
