---
created: 2026-10-17
refs:
  - file: examples/graphics/rotating-cube/rotating_cube.cc
  - file: examples/graphics/sgl-cube/sgl_cube.cc
  - file: examples/graphics/slug-cube/slug_cube.cc
  - file: examples/vdoc/cube-editor/camera.hh
  - file: libs/base/typed-geometry/tests/transform/projective-test.cc
  - file: libs/base/typed-geometry/docs/modules/transform.md
closed: 2026-10-17
resolution: fixed
---

# Where do `perspective` / `look_at` live?

Four examples and one tg test each carry their own `perspective` and `look_at`: left-handed, z into [0, 1], written against `tg::mat`'s column-major `m[col, row]`.
The examples' comments said they belong in tg's transform module.
[transform.md](../../../../modules/transform.md) says the opposite: camera and projection conventions are a rendering convention, and the transform module picks no handedness or depth range.
The one convention tg reads is `frustum::make_from_view_projection`, which assumes reverse-Z [0, 1].

To decide: named factories in tg per convention (as the frustum already does for its depth range), a camera module elsewhere (sr?), or leaving each caller its own.
