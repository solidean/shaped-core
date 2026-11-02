---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_bindings.sgl
---

# A module exports only the structs its own bindings place

Module `scene` declares no binding, so `tracer`'s buffers of its structs would have no C++ type.
shaped-viewer therefore restates `scene`'s structs as `tracer.*_record` in its tracer bindings.
