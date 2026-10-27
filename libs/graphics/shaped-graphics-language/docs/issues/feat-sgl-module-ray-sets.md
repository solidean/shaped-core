---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/shaders/tracer_pipeline.sgl
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_bindings.sgl
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/shader_generator.cc
---

# A ray set a module declares is named by no other file

`rays = tracer.path_rays` is `invalid-pipeline`, a trace of `tracer.path_rays.surface` is refused, and `sgl describe` lists no module's set for a file joining the module.
So shaped-viewer's pipeline file and each generated hit group restate `path_rays` over the module's payloads, which slib holds to the same names, sizes and shapes.
