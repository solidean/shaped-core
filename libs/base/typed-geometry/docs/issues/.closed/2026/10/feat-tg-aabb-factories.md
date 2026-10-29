---
created: 2026-10-17
refs:
  - symbol: babel::gltf::accessor
  - file: libs/data/babel-serializer/src/babel-serializer/geometry/gltf.hh
closed: 2026-10-17
resolution: fixed
---

# `tg::aabb` factories, so stated bounds can come back typed

Wanted: an `aabb` factory from a min / max pair, and eventually from a point range.

A glTF accessor may state `min` / `max`, and for `POSITION` that is a bounding box the caller gets without touching a single buffer byte.
It is the cheapest useful thing in the format — fit a camera, cull, size a scene — and it wants to be a `tg::aabb3f`.

Today `data::min_of` / `max_of` hand back a `cc::span<f32 const>` of `component_count()` floats.
That is honest and dimension-agnostic, since bounds exist for `VEC2` and `MAT4` accessors too, but untyped, so the caller assembles the box.

## Resolution

The min / max factory was the explicit constructor all along.
What landed is `tg::aabb_of(...)` and `tg::aabbX::of(...)`, the box around points, objects with `bounds()` and ranges of either, with the empty box as the default and the identity of union.
babel's `gltf::data::bounds_of(accessor)` returns a stated vec3 bound as a `tg::aabb3f`.
