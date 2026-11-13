---
created: 2026-11-09
priority: next
refs:
  - test: "sg - an SGL ray-tracing pipeline's raygen, closest hit and miss each report a failed assert"
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/msl.cc
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_command_list.cc
closed: 2026-11-10
resolution: fixed
---

# A metal ray-tracing pipeline's stages print and assert through what their raygen hands them, run on a Mac

For Julius, who has the Mac: written on 2026-11-09 on Windows, where neither MSL nor metal can run.
It is the last ⚠️ of matrix row E4, which turns ✅ once this passes on a Mac.

## What changed

- On metal a ray-tracing pipeline's raygen takes `sg_debug` at `[[buffer(30)]]` wherever the compile has diagnostics, whether it prints or not.
- It hands the window on in the new `sgl_context::debug`, and a miss, closest hit, callable, any hit or intersection that prints reads it from there (EMIT-183).
- sg's `raytracing_bind_pipeline` on metal binds the window as a compute or raster bind does, where it had set none.
- SGL's metal entry of `sg_debug` now reaches every stage, so a metal ray-tracing stage no longer drops its prints.

## What is known

- The MSL text is checked by `sgl emit - a metal raygen hands its debug buffer to every ray-tracing stage it calls, which prints through it (EMIT-183)`.
- The GPU test passes on dx12 and vulkan, with and without the native routes; nothing of it has compiled as MSL yet.
- Every metal raygen kernel's signature gained a parameter, so the viewer's path tracer is worth a run too.

## To do on the Mac

1. `uv run dev.py test "each report a failed assert"`: each of the three asserts is logged exactly once on metal.
2. `uv run dev.py test shaped-graphics-test` and `uv run dev.py test shaped-viewer-test`: nothing else of metal's ray tracing moved.
3. On a pass, mark E4 ✅ in [matrix.md](../../../../comparison/matrix.md) and close this.

## Resolved

Run on an M4 on 2026-11-10: the test passed with each of the three asserts logged exactly once, as its `nx::exactly(1)` demands, and it ran rather than skipped.
shaped-graphics-test (626 tests), shaped-graphics-metal-test (99) and shaped-viewer-test (509) passed on metal on the same tree.
E4 is ✅ in the matrix.
