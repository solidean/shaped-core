---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/context.cc
  - file: libs/graphics/shaped-viewer/tests/cross-backend-trace-test.cc
---

# The path tracer is traced on dx12 and vulkan alone

The viewer's default context and the examples are dx12's.
metal has the ray-tracing pipeline the tracer needs and has never run it.
