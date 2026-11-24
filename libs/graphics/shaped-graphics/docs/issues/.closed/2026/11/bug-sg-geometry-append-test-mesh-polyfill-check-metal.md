---
created: 2026-11-23
refs:
  - file: libs/graphics/shaped-graphics/tests/pipeline/vertex-pipeline-atomics-test.cc
  - test: sg - a geometry stage appends one record per primitive at the slot its atomic add claims
closed: 2026-11-24
resolution: fixed
---

# The geometry append test expects a mesh polyfill behind every polyfilled geometry stage, which metal does not build

What is wrong or wanted, and what is already known.

The test fails on an Apple M4 at b1a481919 (2026-11-23), every run, polyfilled arm only:
`(pipeline->mesh_polyfill() != nullptr) == (use == sg::polyfill_use::always)` is `false == true`.
The check came with 14faec936; nothing in the metal backend changed with it.

- **Hypothesis, unmeasured:** metal draws a polyfilled geometry stage as a native Metal mesh pipeline, so no compute-route mesh polyfill stands behind it and `mesh_polyfill()` is null.
  If so, the check should ask whether the context polyfills mesh stages too (`draws_mesh_polyfill()`), not assume it does.
- It is the only failing check of the test's 16: the appended records themselves come out right on metal.

## Resolution

2026-11-24: the hypothesis holds.
Metal reports `feature::mesh_shader` and `draws_geometry_mesh_route`, and the route defaults to `mesh`, so the polyfilled pipeline has `geometry_mesh_corners() > 0` and no `mesh_polyfill()`.
The check now counts either route as polyfilled; the test passes on the M4, 16 of 16 checks.
