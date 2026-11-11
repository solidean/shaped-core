---
created: 2026-11-11
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/stage-polyfills.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/gpu-driven-work.md
  - file: libs/graphics/shaped-graphics-language/docs/comparison/matrix.md
---

# Every stage and GPU-driven work on every target: native where supported, polyfilled where not

Tessellation, geometry, mesh and task stages, work graphs and work lists, on dx12, vulkan, metal and webgpu.
Each one is native where the target has it, and polyfilled where it does not.
This is row D6 of the [comparison matrix](../comparison/matrix.md), whose SGL cell today is ⚠️ for metal tessellation, every polyfill and work graphs.

**Whether a polyfill is automatic or opt-in is decided by measurement.**
Each polyfill lands with a benchmark against the native path on a device that has both.
Within a small factor, it is automatic and its feature stops refusing; further off, a program or a host asks for it by name.
[stage-polyfills.md](../spec/incubator/stage-polyfills.md#automatic-or-opt-in-by-measurement) states the rule.

**Polyfills are open to creative routes**, not only a compute pass ahead of a draw.
A geometry stage may become a mesh stage on metal, and divergent compute or a work graph may run as a raygen that reorders with SER.
[gpu-driven-work.md](../spec/incubator/gpu-driven-work.md) lists the candidates.

Children, native first where there is a native:

- [feat-sgl-metal-tessellation](feat-sgl-metal-tessellation.md): native, on sg's [feat-sg-metal-tessellation](../../../shaped-graphics/docs/issues/feat-sg-metal-tessellation.md).
- [feat-sgl-geometry-stage-polyfill](feat-sgl-geometry-stage-polyfill.md): metal and webgpu.
- [feat-sgl-tessellation-polyfill](feat-sgl-tessellation-polyfill.md): webgpu.
- [feat-sgl-mesh-task-polyfill](feat-sgl-mesh-task-polyfill.md): webgpu.
- [feat-sgl-work-graphs](feat-sgl-work-graphs.md): the language model, native on dx12, on sg's [feat-sg-work-graphs](../../../shaped-graphics/docs/issues/feat-sg-work-graphs.md).
- [feat-sgl-work-graph-polyfill](feat-sgl-work-graph-polyfill.md): every target without them.
- [feat-sgl-work-lists](feat-sgl-work-lists.md): native on three targets, on sg's [feat-sg-device-generated-commands](../../../shaped-graphics/docs/issues/feat-sg-device-generated-commands.md).
- [question-sgl-raygen-ser-scheduler](question-sgl-raygen-ser-scheduler.md): whether a raygen with SER beats compute for divergent work.
