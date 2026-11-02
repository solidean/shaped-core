---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/shader_library.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/material_library.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/context.cc
  - file: libs/graphics/shaped-viewer/tests/material-resolution-test.cc
---

# A library hook has no reset, and whether it should have one differs per hook

`acquire_shader_library`, `acquire_material_library` and `acquire_context` each memoize the first *successful* acquire into a function-local static that nothing clears.
So the first success decides for the process.

slib's constraint is real and external: a second `slib::shader_library` would fight the first over the package globals both write into.
So a shader-library reset is not obviously legal.
The material library is scene state with no such constraint.

Whatever lands has to account for tests running concurrently.
`material-resolution-test.cc` already installs its own material library process-wide without `nx::exclusive`, hedging around the race with `CHECK(builds <= 1)`.
